#include <ipc/notification.hpp>

#include <core/debug.hpp>
#include <sync/irq_lock_guard.hpp>
#include <thread/thread.hpp>
#include <execution/vproc.hpp>
#include <uapi/status.h>

namespace kernel::ipc {

NotificationSource::~NotificationSource() noexcept {
    KASSERT(owner_ != nullptr && ops_ != nullptr);
    KASSERT(!attached() && !hook_.is_linked());
}

auto NotificationSource::attached() const noexcept -> bool {
    kernel::sync::IrqLockGuard guard{lock_};
    return notification_ != nullptr;
}

auto NotificationSource::signal() noexcept -> bool {
    kernel::sync::IrqLockGuard guard{lock_};
    return notification_ != nullptr && notification_->signal(badge_);
}

void NotificationSource::reset() noexcept {
    Notification* target{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
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
      relation_(operation::Completion::bind<
          Wait,
          &Wait::complete,
          &Wait::read,
          &Wait::release,
          &Wait::cancel>(*this)) {}

auto Notification::Wait::idle() const noexcept -> bool {
    return state_.load<libk::MemoryOrder::Acquire>() == State::Idle;
}

auto Notification::Wait::complete() const noexcept -> bool {
    return state_.load<libk::MemoryOrder::Acquire>() == State::Ready;
}

void Notification::Wait::begin() noexcept {
    State expected = State::Idle;
    KASSERT((state_.compare_exchange_strong<
        libk::MemoryOrder::AcqRel,
        libk::MemoryOrder::Acquire>(expected, State::Awaiting)));
}

auto Notification::Wait::arm() noexcept -> bool {
    State expected = State::Awaiting;
    if (state_.compare_exchange_strong<
            libk::MemoryOrder::AcqRel,
            libk::MemoryOrder::Acquire>(expected, State::Armed)) {
        return true;
    }
    KASSERT(expected == State::Ready);
    return false;
}

auto Notification::Wait::ready() noexcept -> bool {
    State observed = state_.load<libk::MemoryOrder::Acquire>();
    for (;;) {
        if (observed == State::Ready || observed == State::Idle) {
            return false;
        }
        KASSERT(observed == State::Awaiting || observed == State::Armed);
        if (state_.compare_exchange_weak<
                libk::MemoryOrder::AcqRel,
                libk::MemoryOrder::Acquire>(observed, State::Ready)) {
            return observed == State::Armed;
        }
    }
}

void Notification::Wait::abort() noexcept {
    State expected = State::Awaiting;
    KASSERT((state_.compare_exchange_strong<
        libk::MemoryOrder::AcqRel,
        libk::MemoryOrder::Acquire>(expected, State::Idle)));
}

auto Notification::Wait::read() noexcept -> operation::Result {
    return owner_->finish_wait();
}

void Notification::Wait::release() noexcept {
}

auto Notification::Wait::cancel() noexcept -> bool {
    return owner_->cancel_wait();
}

Notification::Notification() noexcept : wait_(*this) {}

Notification::~Notification() noexcept {
    const Life life = life_.load<libk::MemoryOrder::Acquire>();
    // ObjectPool may destroy an unpublished construction directly. A
    // published object reaches destruction only through Closed.
    KASSERT(life == Life::Open || life == Life::Closed);
    KASSERT(signalers_.load<libk::MemoryOrder::Acquire>() == 0);
    KASSERT(relations_.load<libk::MemoryOrder::Acquire>() == 0);
    KASSERT(wait_.idle() && sources_.empty() && !cleanup_);
}

auto Notification::signal(u64 badge) noexcept -> bool {
    if (badge == 0
        || life_.load<libk::MemoryOrder::Acquire>() != Life::Open) {
        return false;
    }
    static_cast<void>(
        signalers_.fetch_add<libk::MemoryOrder::AcqRel>(1));
    if (life_.load<libk::MemoryOrder::Acquire>() != Life::Open) {
        const usize previous =
            signalers_.fetch_sub<libk::MemoryOrder::AcqRel>(1);
        KASSERT(previous != 0);
        if (previous == 1) {
            try_finish_retire();
        }
        return false;
    }

    static_cast<void>(pending_.fetch_or<libk::MemoryOrder::Release>(badge));
    if (!wait_.idle() && wait_.ready()) {
        wait_.relation().signal();
    }

    const usize previous = signalers_.fetch_sub<libk::MemoryOrder::AcqRel>(1);
    KASSERT(previous != 0);
    if (previous == 1
        && life_.load<libk::MemoryOrder::Acquire>() != Life::Open) {
        try_finish_retire();
    }
    return true;
}

auto Notification::take() noexcept
    -> libk::Expected<u64, NotificationError> {
    kernel::sync::IrqLockGuard guard{receiver_lock_};
    if (life_.load<libk::MemoryOrder::Acquire>() != Life::Open) {
        return libk::unexpected(NotificationError::Closed);
    }
    if (!wait_.idle()) {
        return libk::unexpected(NotificationError::Busy);
    }
    const u64 badges = pending_.exchange<libk::MemoryOrder::AcqRel>(0);
    return badges != 0
        ? libk::Expected<u64, NotificationError>{libk::expected(badges)}
        : libk::Expected<u64, NotificationError>{
              libk::unexpected(NotificationError::Empty)};
}

auto Notification::wait(Thread& thread, CpuRegistry& cpus) noexcept
    -> libk::Expected<NotificationWait, NotificationError> {
    {
        kernel::sync::IrqLockGuard guard{receiver_lock_};
        if (life_.load<libk::MemoryOrder::Acquire>() != Life::Open) {
            return libk::unexpected(NotificationError::Closed);
        }
        if (!wait_.idle()) {
            return libk::unexpected(NotificationError::Busy);
        }
        const u64 badges = pending_.exchange<libk::MemoryOrder::AcqRel>(0);
        if (badges != 0) {
            return libk::expected(NotificationWait{
                operation::State::Complete, badges, {}});
        }
        wait_.begin();
        if (!thread.begin_wait(wait_.relation(), cpus)) {
            wait_.abort();
            return libk::unexpected(NotificationError::Busy);
        }
    }
    return libk::expected(NotificationWait{
        wait_.arm() ? operation::State::Waiting : operation::State::Complete,
        0,
        {}});
}

auto Notification::wait(
    Vproc& vproc,
    CpuRegistry& cpus,
    usize cookie) noexcept
    -> libk::Expected<NotificationWait, NotificationError> {
    operation::Key key{};
    {
        kernel::sync::IrqLockGuard guard{receiver_lock_};
        if (life_.load<libk::MemoryOrder::Acquire>() != Life::Open) {
            return libk::unexpected(NotificationError::Closed);
        }
        if (!wait_.idle()) {
            return libk::unexpected(NotificationError::Busy);
        }
        const u64 badges = pending_.exchange<libk::MemoryOrder::AcqRel>(0);
        if (badges != 0) {
            return libk::expected(NotificationWait{
                operation::State::Complete, badges, {}});
        }
        wait_.begin();
        auto allocated = vproc.begin_operation(wait_.relation(), cpus, cookie);
        if (!allocated) {
            wait_.abort();
            return libk::unexpected(NotificationError::Busy);
        }
        key = allocated.value();
    }
    if (!wait_.arm()) {
        wait_.relation().signal();
    }
    return libk::expected(NotificationWait{
        operation::State::Waiting, 0, key});
}

auto Notification::bind(NotificationSource& source, u64 badge) noexcept
    -> libk::Expected<void, NotificationError> {
    if (badge == 0) {
        return libk::unexpected(NotificationError::InvalidBadge);
    }
    kernel::sync::IrqLockGuard receiver{receiver_lock_};
    if (life_.load<libk::MemoryOrder::Acquire>() != Life::Open) {
        return libk::unexpected(NotificationError::Closed);
    }
    kernel::sync::IrqLockGuard source_guard{source.lock_};
    if (source.notification_ != nullptr) {
        return libk::unexpected(NotificationError::Busy);
    }
    source.notification_ = this;
    source.badge_ = badge;
    sources_.push_back(source);
    return libk::expected();
}

void Notification::detach_source(
    NotificationSource& source,
    bool notify) noexcept {
    void* owner{};
    const NotificationSource::Ops* ops{};
    {
        kernel::sync::IrqLockGuard receiver{receiver_lock_};
        kernel::sync::IrqLockGuard source_guard{source.lock_};
        if (source.notification_ != this) {
            return;
        }
        KASSERT(source.hook_.is_linked());
        sources_.erase(source);
        source.notification_ = nullptr;
        source.badge_ = 0;
        owner = source.owner_;
        ops = source.ops_;
    }
    if (notify) {
        ops->closed(owner);
    }
}

auto Notification::finish_wait() noexcept -> operation::Result {
    object::ObjectCleanup cleanup{};
    myos_status_t status{MYOS_STATUS_OK};
    u64 badges{};
    {
        kernel::sync::IrqLockGuard guard{receiver_lock_};
        KASSERT(wait_.complete());
        if (life_.load<libk::MemoryOrder::Acquire>() == Life::Open) {
            badges = pending_.exchange<libk::MemoryOrder::AcqRel>(0);
            KASSERT(badges != 0);
        } else {
            status = MYOS_STATUS_CLOSED;
        }
        wait_.state_.store<libk::MemoryOrder::Release>(Wait::State::Idle);
        if (life_.load<libk::MemoryOrder::Acquire>() == Life::Closing
            && signalers_.load<libk::MemoryOrder::Acquire>() == 0
            && relations_.load<libk::MemoryOrder::Acquire>() == 0
            && sources_.empty()) {
            life_.store<libk::MemoryOrder::Release>(Life::Closed);
            cleanup = libk::move(cleanup_);
        }
    }
    if (cleanup) {
        cleanup.complete();
    }
    return operation::Result{status, badges};
}

auto Notification::cancel_wait() noexcept -> bool {
    kernel::sync::IrqLockGuard guard{receiver_lock_};
    Wait::State observed =
        wait_.state_.load<libk::MemoryOrder::Acquire>();
    for (;;) {
        if (observed == Wait::State::Ready) {
            return false;
        }
        KASSERT(observed == Wait::State::Awaiting
            || observed == Wait::State::Armed);
        if (wait_.state_.compare_exchange_weak<
                libk::MemoryOrder::AcqRel,
                libk::MemoryOrder::Acquire>(observed, Wait::State::Idle)) {
            return true;
        }
    }
}

void Notification::retire(object::ObjectCleanup&& cleanup) noexcept {
    Life expected = Life::Open;
    KASSERT((life_.compare_exchange_strong<
        libk::MemoryOrder::AcqRel,
        libk::MemoryOrder::Acquire>(expected, Life::Closing)));
    {
        kernel::sync::IrqLockGuard guard{receiver_lock_};
        KASSERT(!cleanup_);
        cleanup_ = libk::move(cleanup);
        if (!wait_.idle() && wait_.ready()) {
            wait_.relation().signal();
        }
    }

    for (;;) {
        void* owner{};
        const NotificationSource::Ops* ops{};
        {
            kernel::sync::IrqLockGuard guard{receiver_lock_};
            if (sources_.empty()) {
                break;
            }
            NotificationSource& source = sources_.front();
            kernel::sync::IrqLockGuard source_guard{source.lock_};
            KASSERT(source.notification_ == this);
            sources_.erase(source);
            source.notification_ = nullptr;
            source.badge_ = 0;
            owner = source.owner_;
            ops = source.ops_;
        }
        ops->closed(owner);
    }
    try_finish_retire();
}

void Notification::retain_relation() noexcept {
    const usize previous =
        relations_.fetch_add<libk::MemoryOrder::AcqRel>(1);
    KASSERT(previous != static_cast<usize>(-1));
}

void Notification::release_relation() noexcept {
    const usize previous =
        relations_.fetch_sub<libk::MemoryOrder::AcqRel>(1);
    KASSERT(previous != 0);
    if (previous == 1
        && life_.load<libk::MemoryOrder::Acquire>() != Life::Open) {
        try_finish_retire();
    }
}

void Notification::try_finish_retire() noexcept {
    object::ObjectCleanup cleanup{};
    {
        kernel::sync::IrqLockGuard guard{receiver_lock_};
        if (life_.load<libk::MemoryOrder::Acquire>() != Life::Closing
            || signalers_.load<libk::MemoryOrder::Acquire>() != 0
            || relations_.load<libk::MemoryOrder::Acquire>() != 0
            || !wait_.idle() || !sources_.empty() || !cleanup_) {
            return;
        }
        life_.store<libk::MemoryOrder::Release>(Life::Closed);
        cleanup = libk::move(cleanup_);
    }
    cleanup.complete();
}

} // namespace kernel::ipc
