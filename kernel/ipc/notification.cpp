#include <expected>
#include <optional>
#include <ipc/notification.hpp>
#include <object/ref.hpp>

#include <libk/assert.hpp>
#include <base/types.hpp>
#include <cpu/registry.hpp>
#include <cpu/local.hpp>
#include <sched/dispatcher.hpp>
#include <limits>
#include <utility>
#include <sched/sc.hpp>
#include <sync.hpp>
#include <task/thread.hpp>
#include <uapi/status.h>

namespace ipc {

NotificationSource::~NotificationSource() noexcept {
    libk_assert(!attached() && !hook_.is_linked());
}

auto NotificationSource::attached() const noexcept -> bool {
    sync::Lock guard{lock_};
    return notification_ != nullptr;
}

auto NotificationSource::signal() noexcept -> bool {
    Notification* target{};
    u64 badge{};
    {
        sync::Lock guard{lock_};
        target = notification_;
        badge = badge_;
        if (target != nullptr) {
            target->retain_relation();
        }
    }
    if (target == nullptr) {
        return false;
    }
    const bool delivered = target->signal(badge);
    target->release_relation();
    return delivered;
}

void NotificationSource::reset() noexcept {
    Notification* target{};
    {
        sync::Lock guard{lock_};
        target = notification_;
        if (target != nullptr) {
            // The source lock prevents retirement from detaching this edge
            // until the transient call lease has been published.
            target->retain_relation();
        }
    }
    if (target != nullptr) {
        target->detach_source(*this, false);
        target->release_relation();
    }
}

Notification::Wait::Wait(Notification& owner) noexcept
    : owner_(&owner),
      relation_(Completion::bind<Wait, &Wait::release, &Wait::cancel>(*this)),
      deadline_(sched::Deadline::Callback::bind<&Wait::expire>(*this)) {}

Notification::Wait::~Wait() noexcept {
    libk_assert(!hook_.is_linked() && !relation_.attached() && !deadline_.armed());
}

auto Notification::Wait::ready(WaitResult result) noexcept -> bool {
    if (!hook_.is_linked()) return false;
    owner_->waiters_.erase(*this);
    result_ = result;
    return true;
}

void Notification::Wait::release() noexcept {
    if (deadline_.armed()) current_cpu().dispatcher()->disarm(deadline_);
    owner_->release_wait();
}

auto Notification::Wait::cancel() noexcept -> bool {
    sync::Lock guard{owner_->receiver_lock_};
    return ready({MYOS_STATUS_CANCELED, 0});
}

void Notification::Wait::expire() noexcept {
    bool publish{};
    {
        sync::Lock guard{owner_->receiver_lock_};
        publish = ready({MYOS_STATUS_TIMED_OUT, 0});
    }
    if (publish) relation_.signal();
}

Notification::~Notification() noexcept {
    const Life life = life_.load<libk::MemoryOrder::Acquire>();
    // pool may destroy an unpublished construction directly. A
    // published object reaches destruction only through Closed.
    libk_assert(life == Life::Open || life == Life::Closed);
    libk_assert(signalers_.load<libk::MemoryOrder::Acquire>() == 0);
    libk_assert(relations_.load<libk::MemoryOrder::Acquire>() == 0);
    libk_assert(waiter_count_ == 0 && waiters_.empty() && sources_.empty() && !cleanup_);

}

auto Notification::signal(u64 badge) noexcept -> bool {
    if (!badge || life_.load<libk::MemoryOrder::Acquire>() != Life::Open) return false;
    static_cast<void>(signalers_.fetch_add<libk::MemoryOrder::AcqRel>(1));
    bool accepted{};
    Wait* waiter{};
    {
        sync::Lock guard{receiver_lock_};
        if (life_.load<libk::MemoryOrder::Acquire>() == Life::Open) {
            pending_ |= badge;
            libk_assert(signal_sequence_ != std::numeric_limits<u64>::max());
            ++signal_sequence_;
            accepted = true;
            if (!waiters_.empty()) {
                waiter = &waiters_.front();
                libk_assert(waiter->ready({MYOS_STATUS_OK,
                    std::exchange(pending_, u64{})}));
            }
        }
    }
    const usize n = signalers_.fetch_sub<libk::MemoryOrder::AcqRel>(1);
    libk_assert(n);
    if (n == 1 && life_.load<libk::MemoryOrder::Acquire>() != Life::Open) try_finish_retire();
    // waiter_count_ pins the object; the selected result excludes cancellation.
    // Publication is the final access to stack-owned waiter storage.
    if (waiter != nullptr) waiter->relation_.signal();
    return accepted;
}

auto Notification::take() noexcept -> std::expected<NotificationTake, NotificationError> {
    sync::Lock guard{receiver_lock_};
    if (life_.load<libk::MemoryOrder::Acquire>() != Life::Open) return std::unexpected(NotificationError::Closed);
    const auto badges = std::exchange(pending_, u64{});
    if (!badges) return std::unexpected(NotificationError::Empty);
    return (NotificationTake{badges, signal_sequence_});
}

auto Notification::wait(Thread& thread, CpuRegistry& cpus,
                        sched::Dispatcher& dispatcher,
                        std::optional<time::Instant> deadline) noexcept -> WaitResult {
    Wait waiter{*this};
    {
        sync::Lock guard{receiver_lock_};
        if (life_.load<libk::MemoryOrder::Acquire>() != Life::Open) return {MYOS_STATUS_CLOSED, 0};
        const u64 badges = std::exchange(pending_, u64{});
        if (badges != 0) return {MYOS_STATUS_OK, badges};
        if (deadline && !dispatcher.arm(waiter.deadline_, *deadline)) return {MYOS_STATUS_BUSY, 0};
        if (!thread.begin_wait(waiter.relation_, cpus)) {
            if (waiter.deadline_.armed()) dispatcher.disarm(waiter.deadline_);
            return {MYOS_STATUS_BUSY, 0};
        }
        waiters_.push_back(waiter);
        ++waiter_count_;
    }
    thread.block();
    if (thread.stop_requested()) return {MYOS_STATUS_CANCELED, 0};
    return waiter.result_;
}

auto Notification::bind(NotificationSource& source, u64 badge) noexcept
    -> std::expected<void, NotificationError> {
    if (badge == 0) {
        return std::unexpected(NotificationError::InvalidBadge);
    }
    sync::Lock receiver{receiver_lock_};
    if (life_.load<libk::MemoryOrder::Acquire>() != Life::Open) {
        return std::unexpected(NotificationError::Closed);
    }
    sync::Lock source_guard{source.lock_};
    if (source.notification_ != nullptr) {
        return std::unexpected(NotificationError::Busy);
    }
    source.notification_ = this;
    source.badge_ = badge;
    sources_.push_back(source);
    return {};
}

void Notification::detach_source(
    NotificationSource& source,
    bool notify) noexcept {
    NotificationSource::Closed closed{};
    {
        sync::Lock receiver{receiver_lock_};
        sync::Lock source_guard{source.lock_};
        if (source.notification_ != this) {
            return;
        }
        libk_assert(source.hook_.is_linked());
        sources_.erase(source);
        source.notification_ = nullptr;
        source.badge_ = 0;
        closed = source.closed_;
    }
    if (notify && closed) closed();
}

void Notification::release_wait() noexcept {
    {
        sync::Lock guard{receiver_lock_};
        libk_assert(waiter_count_ != 0);
        --waiter_count_;
    }
    try_finish_retire();
}

void Notification::retire(object::cleanup&& cleanup) noexcept {
    // The retirement publisher survives wakes, source callbacks and cleanup.
    retain_relation();
    Life expected = Life::Open;
    libk_assert((life_.compare_exchange_strong<libk::MemoryOrder::AcqRel,
        libk::MemoryOrder::Acquire>(expected, Life::Closing)));
    {
        sync::Lock guard{receiver_lock_};
        libk_assert(!cleanup_);
        cleanup_ = std::move(cleanup);
    }
    for (;;) {
        Wait* waiter{};
        {
            sync::Lock guard{receiver_lock_};
            if (waiters_.empty()) break;
            waiter = &waiters_.front();
            libk_assert(waiter->ready({MYOS_STATUS_CLOSED, 0}));
        }
        waiter->relation_.signal();
    }
    for (;;) {
        NotificationSource::Closed closed{};
        {
            sync::Lock guard{receiver_lock_};
            if (sources_.empty()) break;
            NotificationSource& source = sources_.front();
            sync::Lock source_guard{source.lock_};
            libk_assert(source.notification_ == this);
            sources_.erase(source);
            source.notification_ = nullptr;
            source.badge_ = 0;
            closed = source.closed_;
        }
        if (closed) closed();
    }
    release_relation();
}

void Notification::retain_relation() noexcept {
    const usize previous =
        relations_.fetch_add<libk::MemoryOrder::AcqRel>(1);
    libk_assert(previous != static_cast<usize>(-1));
}

void Notification::release_relation() noexcept {
    const usize previous =
        relations_.fetch_sub<libk::MemoryOrder::AcqRel>(1);
    libk_assert(previous != 0);
    if (previous == 1
        && life_.load<libk::MemoryOrder::Acquire>() != Life::Open) {
        try_finish_retire();
    }
}

void Notification::try_finish_retire() noexcept {
    object::cleanup cleanup{};
    {
        sync::Lock guard{receiver_lock_};
        if (life_.load<libk::MemoryOrder::Acquire>() != Life::Closing
            || signalers_.load<libk::MemoryOrder::Acquire>() != 0
            || relations_.load<libk::MemoryOrder::Acquire>() != 0
            || waiter_count_ != 0 || !sources_.empty()
            || !cleanup_) {
            return;
        }
        life_.store<libk::MemoryOrder::Release>(Life::Closed);
        cleanup = std::move(cleanup_);
    }
    cleanup.complete();
}

} // namespace ipc
