#include <wait.hpp>
#include <trace.hpp>
#include <libk/assert.hpp>
#include <base/types.hpp>
#include <cpu/cpu.hpp>
#include <sched/sc.hpp>
#include <sched/dispatcher.hpp>
#include <sched/guard.hpp>
#include <sync.hpp>

Completion::~Completion() noexcept {
    libk_assert(owner_ != nullptr && ops_ != nullptr);
    libk_assert(!attached());
}

void Completion::attach(Wait& wait) noexcept {
    libk_assert(!attached());
    wait_ = &wait;
    trace::emit(trace::Event::Attach, reinterpret_cast<u64>(&wait), reinterpret_cast<u64>(this));
    delivery_.store<libk::MemoryOrder::Release>(Delivery::Attached);
}

void Completion::signal() noexcept {
    // Claim publication with one atomic state.  Cancellation owns the edge
    // while it decides whether the operation can be canceled; a producer that
    // meets that owner records a durable race.  If cancellation wins its
    // reopen CAS first, this loop simply claims the reattached generation.
    for (;;) {
        Delivery expected = delivery_.load<libk::MemoryOrder::Acquire>();
        if (expected == Delivery::Attached) {
            if (delivery_.compare_exchange_strong<
                    libk::MemoryOrder::AcqRel,
                    libk::MemoryOrder::Acquire>(expected, Delivery::Claimed)) {
                break;
            }
            continue;
        }
        if (expected == Delivery::Cancelling) {
            if (delivery_.compare_exchange_strong<
                    libk::MemoryOrder::AcqRel,
                    libk::MemoryOrder::Acquire>(
                        expected, Delivery::CancelRaced)) {
                return;
            }
            continue;
        }
        libk_assert(expected == Delivery::Claimed
            || expected == Delivery::Ready
            || expected == Delivery::Detached
            || expected == Delivery::CancelRaced);
        return;
    }
    const u64 actor = reinterpret_cast<u64>(owner_);
    const u64 id = reinterpret_cast<u64>(this);
    trace::emit(trace::Event::Claim, actor, id);
    sched::PreemptGuard preempt;
    wait_->wake();
    delivery_.store<libk::MemoryOrder::Release>(Delivery::Ready);
    trace::emit(trace::Event::Complete, actor, id);

}

auto Completion::try_claim_finish() noexcept -> FinishClaim {
    Delivery expected = delivery_.load<libk::MemoryOrder::Acquire>();
    if (expected == Delivery::Claimed) {
        return FinishClaim::Publishing;
    }
    if (expected != Delivery::Ready) {
        return FinishClaim::Unavailable;
    }
    if (delivery_.compare_exchange_strong<
            libk::MemoryOrder::AcqRel,
            libk::MemoryOrder::Acquire>(expected, Delivery::Claimed)) {
        return FinishClaim::Claimed;
    }
    return expected == Delivery::Claimed
        ? FinishClaim::Publishing : FinishClaim::Unavailable;
}

auto Completion::try_claim_cancel() noexcept -> CancelClaim {
    const Delivery observed = delivery_.load<libk::MemoryOrder::Acquire>();
    if (observed != Delivery::Attached && observed != Delivery::Ready) return CancelClaim::Unavailable;
    Delivery expected = observed;
    if (!delivery_.compare_exchange_strong<libk::MemoryOrder::AcqRel, libk::MemoryOrder::Acquire>(
            expected, Delivery::Cancelling)) return CancelClaim::Unavailable;
    return observed == Delivery::Ready ? CancelClaim::Published : CancelClaim::Pending;
}

auto Completion::resolve_cancel(CancelClaim claim) noexcept -> CancelResult {
    libk_assert(claim != CancelClaim::Unavailable);
    if (claim == CancelClaim::Published) return CancelResult::Completed;
    if (ops_->cancel(owner_)) return CancelResult::Canceled;
    // A ready result is not yet a released producer. Until signal() publishes
    // Ready or CancelRaced, its callback may still access this operation.
    return CancelResult::Reopen;
}

auto Completion::try_reopen_cancel() noexcept -> bool {
    Delivery expected = Delivery::Cancelling;
    const bool reopened = delivery_.compare_exchange_strong<
        libk::MemoryOrder::AcqRel,
        libk::MemoryOrder::Acquire>(expected, Delivery::Attached);
    if (!reopened) {
        libk_assert(expected == Delivery::CancelRaced);
    }
    return reopened;
}

void Completion::finish_claimed() noexcept {
    libk_assert(delivery_.load<libk::MemoryOrder::Acquire>() == Delivery::Claimed);
    const Ops* const ops = ops_;
    void* const owner = owner_;
    const u64 actor = reinterpret_cast<u64>(owner);
    const u64 id = reinterpret_cast<u64>(this);
    wait_ = nullptr;
    // Final member access: release may destroy this Completion.
    delivery_.store<libk::MemoryOrder::Release>(Delivery::Detached);
    ops->release(owner);
    trace::emit(trace::Event::Finish, actor, id);
}

void Completion::finalize_cancel(CancelResult result) noexcept {
    libk_assert(result != CancelResult::Reopen);
    const Delivery state = delivery_.load<libk::MemoryOrder::Acquire>();
    libk_assert(state == Delivery::Cancelling || state == Delivery::CancelRaced);
    const Ops* const ops = ops_;
    void* const owner = owner_;
    const u64 actor = reinterpret_cast<u64>(owner);
    const u64 id = reinterpret_cast<u64>(this);
    wait_ = nullptr;
    delivery_.store<libk::MemoryOrder::Release>(Delivery::Detached);
    ops->release(owner);
    trace::emit(trace::Event::Cancel, actor, id, static_cast<u64>(result));
}

Wait::Wait() noexcept = default;

Wait::~Wait() noexcept {
    libk_assert(!attached());
}

auto Wait::attached() const noexcept -> bool {
    sync::Lock guard{lock_};
    return phase_ != EdgePhase::Detached;
}

auto Wait::ready() const noexcept -> bool {
    return ready_.load<libk::MemoryOrder::Acquire>();
}

auto Wait::begin(
    Completion& completion,
    Cpus& cpus,
    sched::Sc& binding) noexcept -> bool {
    {
        sync::Lock guard{lock_};
        if (phase_ != EdgePhase::Detached || completion.attached()) {
            return false;
        }
        completion_ = &completion;
        cpus_ = &cpus;
        binding_ = &binding;
        ready_.store<libk::MemoryOrder::Relaxed>(false);
        completion.attach(*this);

        phase_ = EdgePhase::Attached;
    }
    return true;
}

auto Wait::finish() noexcept -> bool {
    Completion* completion{};
    bool claimed{};
    {
        sync::Lock guard{lock_};
        if (phase_ != EdgePhase::Attached
            || completion_ == nullptr
            || !ready_.load<libk::MemoryOrder::Acquire>()) {
            return false;
        }
        completion = completion_;
        // The Delivery claim is callback-free.  It pins the Completion owner
        // while this Wait edge is unlinked and the terminal callbacks run.
        const Completion::FinishClaim claim =
            completion->try_claim_finish();
        switch (claim) {
        case Completion::FinishClaim::Claimed:
            claimed = true;
            // The Completion CAS transfers terminal ownership, but the Wait
            // edge must publish the same ownership under its container lock
            // before that lock is released.  This closes the interval in
            // which cancel() or a re-entrant finisher could still observe an
            // Attached edge while the terminal owner is already proceeding.
            phase_ = EdgePhase::FinishOwned;
            break;
        case Completion::FinishClaim::Publishing:
            // The producer has already claimed publication.  Retain the
            // borrowed pointer under a distinct Wait phase while the
            // producer completes its wake-to-Ready handoff; cancellation and
            // a second finisher now lose without touching Completion.
            phase_ = EdgePhase::FinishOwned;
            break;
        case Completion::FinishClaim::Unavailable:
            libk_assert(false);
            return false;
        }
    }

    if (!claimed) {
        for (;;) {
            const Completion::FinishClaim claim =
                completion->try_claim_finish();
            if (claim == Completion::FinishClaim::Claimed) {
                break;
            }
            // FinishOwned excludes cancellation and all other finishers;
            // Publishing is the only legal transient state here.  The
            // BlockingSink producer is pinned on its CPU by PreemptGuard, so
            // same-hart publication cannot self-deadlock, while a remote
            // producer continues independently to Ready.
            libk_assert(claim == Completion::FinishClaim::Publishing);
            libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();

        }
    }

    {
        sync::Lock guard{lock_};
        libk_assert(phase_ == EdgePhase::FinishOwned && completion_ == completion);
        completion_ = nullptr;
        cpus_ = nullptr;
        binding_ = nullptr;
        ready_.store<libk::MemoryOrder::Relaxed>(false);
        phase_ = EdgePhase::Detached;
    }
    completion->finish_claimed();
    return true;
}

auto Wait::cancel() noexcept -> bool {
    Completion* completion{};
    bool was_ready{};
    Completion::CancelClaim claim{};
    {
        sync::Lock guard{lock_};
        if (phase_ != EdgePhase::Attached || completion_ == nullptr) {
            return false;
        }
        completion = completion_;
        was_ready = ready_.load<libk::MemoryOrder::Acquire>();
        // Keep the pointer and its exact edge fields in the container while
        // cancellation resolves policy outside the lock.  Delivery now pins
        // the owner against producer publication and finish.
        claim = completion->try_claim_cancel();
        if (claim == Completion::CancelClaim::Unavailable) {
            return false;
        }
        phase_ = EdgePhase::CancelOwned;
    }

    Completion::CancelResult resolution = completion->resolve_cancel(claim);
    if (resolution == Completion::CancelResult::Reopen) {
        bool reopened{};
        {
            sync::Lock guard{lock_};
            libk_assert(phase_ == EdgePhase::CancelOwned
                && completion_ == completion);
            // Restore the complete borrowed-edge projection before the
            // Cancelling -> Attached publication.  A producer can therefore
            // never claim an edge whose Wait fields are only half restored.
            phase_ = EdgePhase::Attached;
            ready_.store<libk::MemoryOrder::Relaxed>(was_ready);
            reopened = completion->try_reopen_cancel();
            if (!reopened) {
                // The only legal loser is CancelRaced: the producer left a
                // durable handoff and this cancellation owner must drain it.
                phase_ = EdgePhase::CancelOwned;
            }
        }
        if (reopened) {
            return false;
        }
        resolution = Completion::CancelResult::Completed;
    }

    {
        sync::Lock guard{lock_};
        libk_assert(phase_ == EdgePhase::CancelOwned
            && completion_ == completion);
        completion_ = nullptr;
        cpus_ = nullptr;
        binding_ = nullptr;
        ready_.store<libk::MemoryOrder::Relaxed>(false);
        phase_ = EdgePhase::Detached;
    }
    completion->finalize_cancel(resolution);
    return true;
}

void Wait::wake() noexcept {
    Cpus* cpus{};
    sched::Sc* binding{};
    {
        sync::Lock guard{lock_};
        libk_assert(phase_ == EdgePhase::Attached
            && completion_ != nullptr && cpus_ != nullptr);
        ready_.store<libk::MemoryOrder::Release>(true);
        cpus = cpus_;
        binding = binding_;
        libk_assert(binding != nullptr);
    }
    libk_assert(binding && sched::wake(*cpus, *binding));
}

Receipt::Receipt() noexcept
    : completion_(Completion::bind<Receipt,
          &Receipt::release, &Receipt::cancel>(*this)) {}

void Receipt::signal() noexcept {
    libk_assert(committed_);
    done_.store<libk::MemoryOrder::Release>(true);
    completion_.signal();
}
