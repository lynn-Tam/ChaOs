#include <object/notification_pool.hpp>

#include <core/debug.hpp>
#include <cpu/cpu_registry.hpp>
#include <libk/limits.hpp>
#include <libk/scope_guard.hpp>
#include <sched/binding.hpp>
#include <sync/irq_lock_guard.hpp>
#include <thread/thread.hpp>
#include <execution/vproc.hpp>
#include <uapi/status.h>

namespace kernel::ipc {

const cap::GrantAttachmentOps Notification::authority_ops_{
    .invalidate = &Notification::invalidate,
    .released = &Notification::released,
};

NotificationSource::~NotificationSource() noexcept {
    KASSERT(owner_ != nullptr && ops_ != nullptr);
    KASSERT(!attached() && !hook_.is_linked());
}

auto NotificationSource::attached() const noexcept -> bool {
    kernel::sync::IrqLockGuard guard{lock_};
    return notification_ != nullptr;
}

auto NotificationSource::signal() noexcept -> bool {
    Notification* target{};
    u64 badge{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
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

Notification::Notification() noexcept
    : wait_(*this), target_link_(*this), authority_(*this, authority_ops_) {}

Notification::~Notification() noexcept {
    const Life life = life_.load<libk::MemoryOrder::Acquire>();
    // ObjectPool may destroy an unpublished construction directly. A
    // published object reaches destruction only through Closed.
    KASSERT(life == Life::Open || life == Life::Closed);
    KASSERT(signalers_.load<libk::MemoryOrder::Acquire>() == 0);
    KASSERT(relations_.load<libk::MemoryOrder::Acquire>() == 0);
    KASSERT(wait_.idle() && sources_.empty() && !cleanup_);
    KASSERT(receiver_ == Receiver::None && target_ == nullptr
        && !target_hold_ && async_publishers_ == 0);
    KASSERT(!authority_.attachment.attached()
        && !authority_.attachment.busy() && !authority_.work);
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

    Vproc* target{};
    CpuRegistry* cpus{};
    usize slot{};
    usize tag{};
    u64 generation{};
    u64 sequence{};
    bool wake{};
    bool rejected{};
    {
        kernel::sync::IrqLockGuard guard{receiver_lock_};
        if (life_.load<libk::MemoryOrder::Acquire>() != Life::Open) {
            rejected = true;
        } else {
            static_cast<void>(
                pending_.fetch_or<libk::MemoryOrder::Release>(badge));
            KASSERT(signal_sequence_ != libk::numeric_limits<u64>::max());
            ++signal_sequence_;
            sequence = signal_sequence_;
            if (receiver_ == Receiver::Async) {
                KASSERT(target_ != nullptr && cpus_ != nullptr);
                ++async_publishers_;
                target = target_;
                cpus = cpus_;
                slot = slot_;
                tag = tag_;
                generation = binding_generation_;
            } else if (!wait_.idle()) {
                wake = wait_.ready();
            }
        }
    }
    if (rejected) {
        const usize previous =
            signalers_.fetch_sub<libk::MemoryOrder::AcqRel>(1);
        KASSERT(previous != 0);
        if (previous == 1) {
            try_finish_retire();
        }
        return false;
    }
    if (target != nullptr) {
        target->publish_notification(
            target_link_, slot, generation, sequence, tag, *cpus);
        async_publisher_done();
    } else if (wake) {
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

auto Notification::take(Vproc* current) noexcept
    -> libk::Expected<NotificationTake, NotificationError> {
    Vproc* target{};
    usize slot{};
    u64 generation{};
    u64 sequence{};
    u64 badges{};
    {
        kernel::sync::IrqLockGuard guard{receiver_lock_};
        if (life_.load<libk::MemoryOrder::Acquire>() != Life::Open) {
            return libk::unexpected(NotificationError::Closed);
        }
        if (!wait_.idle()
            || (receiver_ != Receiver::None
                && (receiver_ != Receiver::Async || current != target_))) {
            return libk::unexpected(NotificationError::Busy);
        }
        badges = pending_.exchange<libk::MemoryOrder::AcqRel>(0);
        if (badges == 0) {
            return libk::unexpected(NotificationError::Empty);
        }
        sequence = signal_sequence_;
        if (receiver_ == Receiver::Async) {
            ++async_publishers_;
            target = target_;
            slot = slot_;
            generation = binding_generation_;
        }
    }
    if (target != nullptr) {
        target->clear_notification(
            target_link_, slot, generation, sequence);
        async_publisher_done();
    }
    return libk::expected(NotificationTake{badges, sequence});
}

auto Notification::wait(Thread& thread, CpuRegistry& cpus) noexcept
    -> libk::Expected<NotificationWait, NotificationError> {
    {
        kernel::sync::IrqLockGuard guard{receiver_lock_};
        if (life_.load<libk::MemoryOrder::Acquire>() != Life::Open) {
            return libk::unexpected(NotificationError::Closed);
        }
        if (!wait_.idle() || receiver_ != Receiver::None) {
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

auto Notification::bind_vproc(
    Vproc& vproc,
    CpuRegistry& cpus,
    const cap::Resolved<Notification>& authority,
    usize slot,
    usize tag) noexcept -> libk::Expected<void, NotificationError> {
    if (&authority.object() != this
        || &authority.source() != vproc.execution().binding().cspace()) {
        return libk::unexpected(NotificationError::Busy);
    }
    sched::Binding* const binding = vproc.binding();
    if (binding == nullptr) {
        return libk::unexpected(NotificationError::Busy);
    }
    auto reference = binding->target_reference();
    if (!reference) {
        return libk::unexpected(NotificationError::Busy);
    }
    auto hold = libk::move(reference).value().into_hold<Vproc>();
    if (!hold) {
        return libk::unexpected(NotificationError::Busy);
    }

    {
        kernel::sync::IrqLockGuard guard{receiver_lock_};
        if (life_.load<libk::MemoryOrder::Acquire>() != Life::Open) {
            return libk::unexpected(NotificationError::Closed);
        }
        if (!wait_.idle() || receiver_ != Receiver::None) {
            return libk::unexpected(NotificationError::Busy);
        }
        receiver_ = Receiver::Attaching;
        retain_relation();
    }

    bool reverse_attached{};
    bool authority_attached{};
    u64 generation{};
    auto rollback = libk::on_scope_exit([&]() noexcept {
        if (authority_attached && authority_.attachment.attached()) {
            static_cast<void>(authority_.attachment.detach());
        }
        if (reverse_attached) {
            vproc.detach_notification(target_link_, slot, generation);
        }
        {
            kernel::sync::IrqLockGuard guard{receiver_lock_};
            if (receiver_ == Receiver::Attaching
                || receiver_ == Receiver::Detaching) {
                receiver_ = Receiver::None;
            }
        }
        release_relation();
    });

    const auto attached = vproc.attach_notification(target_link_, slot, tag);
    if (!attached) {
        return libk::unexpected(NotificationError::Busy);
    }
    generation = *attached;
    reverse_attached = true;
    if (!authority.attach(authority_.attachment)) {
        return libk::unexpected(NotificationError::Busy);
    }
    authority_attached = true;

    u64 sequence{};
    bool publish{};
    {
        kernel::sync::IrqLockGuard guard{receiver_lock_};
        if (life_.load<libk::MemoryOrder::Acquire>() != Life::Open
            || receiver_ != Receiver::Attaching) {
            return libk::unexpected(NotificationError::Closed);
        }
        target_ = &vproc;
        target_hold_ = libk::move(hold).value();
        cpus_ = &cpus;
        slot_ = slot;
        tag_ = tag;
        binding_generation_ = generation;
        receiver_ = Receiver::Async;
        if (pending_.load<libk::MemoryOrder::Acquire>() != 0) {
            ++async_publishers_;
            sequence = signal_sequence_;
            publish = true;
        }
    }
    static_cast<void>(rollback.release());
    release_relation();
    if (publish) {
        vproc.publish_notification(
            target_link_, slot, generation, sequence, tag, cpus);
        async_publisher_done();
    }
    return libk::expected();
}

auto Notification::unbind_vproc(Vproc& vproc) noexcept
    -> libk::Expected<void, NotificationError> {
    {
        kernel::sync::IrqLockGuard guard{receiver_lock_};
        if (receiver_ == Receiver::None) {
            return libk::expected();
        }
        if ((receiver_ != Receiver::Async
                && receiver_ != Receiver::Detaching)
            || target_ != &vproc) {
            return libk::unexpected(NotificationError::Busy);
        }
        receiver_ = Receiver::Detaching;
    }
    try_finish_async_detach();
    kernel::sync::IrqLockGuard guard{receiver_lock_};
    return receiver_ == Receiver::None
        ? libk::Expected<void, NotificationError>{libk::expected()}
        : libk::Expected<void, NotificationError>{
              libk::unexpected(NotificationError::Busy)};
}

void Notification::invalidate(
    void* context,
    cap::GrantWork&& work,
    cap::GrantInvalidation reason) noexcept {
    KASSERT(context != nullptr && reason == cap::GrantInvalidation::Revoke);
    auto& authority = *static_cast<AuthorityLink*>(context);
    authority.owner->invalidated(libk::move(work));
}

void Notification::released(void* context) noexcept {
    KASSERT(context != nullptr);
}

void Notification::invalidated(cap::GrantWork&& work) noexcept {
    {
        kernel::sync::IrqLockGuard guard{receiver_lock_};
        KASSERT(!authority_.work);
        authority_.work = libk::move(work);
        if (receiver_ == Receiver::Async
            || receiver_ == Receiver::Attaching) {
            receiver_ = Receiver::Detaching;
        }
    }
    try_finish_async_detach();
}

void Notification::begin_async_detach() noexcept {
    {
        kernel::sync::IrqLockGuard guard{receiver_lock_};
        if (receiver_ == Receiver::Async
            || receiver_ == Receiver::Attaching) {
            receiver_ = Receiver::Detaching;
        }
    }
    try_finish_async_detach();
}

void Notification::try_finish_async_detach() noexcept {
    Vproc* target{};
    object::ObjectHold<Vproc> hold{};
    usize slot{};
    u64 generation{};
    {
        kernel::sync::IrqLockGuard guard{receiver_lock_};
        if (receiver_ != Receiver::Detaching || async_publishers_ != 0) {
            return;
        }
        receiver_ = Receiver::Draining;
        target = target_;
        hold = libk::move(target_hold_);
        slot = slot_;
        generation = binding_generation_;
    }

    if (target != nullptr && generation != 0) {
        target->detach_notification(target_link_, slot, generation);
    }
    if (authority_.attachment.attached()) {
        static_cast<void>(authority_.attachment.detach());
    }

    cap::GrantWork work{};
    {
        kernel::sync::IrqLockGuard guard{receiver_lock_};
        KASSERT(receiver_ == Receiver::Draining);
        work = libk::move(authority_.work);
        target_ = nullptr;
        cpus_ = nullptr;
        slot_ = 0;
        tag_ = 0;
        binding_generation_ = 0;
        receiver_ = Receiver::None;
    }
    work.reset();
    hold.reset();
    try_finish_retire();
}

void Notification::async_publisher_done() noexcept {
    bool finish{};
    {
        kernel::sync::IrqLockGuard guard{receiver_lock_};
        KASSERT(async_publishers_ != 0);
        --async_publishers_;
        finish = async_publishers_ == 0
            && receiver_ == Receiver::Detaching;
    }
    if (finish) {
        try_finish_async_detach();
    }
}

void Notification::peer_stopped(Vproc& vproc) noexcept {
    {
        kernel::sync::IrqLockGuard guard{receiver_lock_};
        if (target_ != &vproc || receiver_ == Receiver::None) {
            return;
        }
        if (receiver_ == Receiver::Async) {
            receiver_ = Receiver::Detaching;
        }
    }
    try_finish_async_detach();
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
            && sources_.empty() && receiver_ == Receiver::None) {
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
    begin_async_detach();

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
            || !wait_.idle() || !sources_.empty()
            || receiver_ != Receiver::None || authority_.work
            || !cleanup_) {
            return;
        }
        life_.store<libk::MemoryOrder::Release>(Life::Closed);
        cleanup = libk::move(cleanup_);
    }
    cleanup.complete();
}

} // namespace kernel::ipc
