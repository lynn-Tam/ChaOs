#include <operation/wait.hpp>

#include <core/debug.hpp>
#include <cpu/cpu_local.hpp>
#include <cpu/cpu_registry.hpp>
#include <sched/dispatcher.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel::operation {

Wait::Wait() noexcept = default;

Wait::~Wait() noexcept {
    KASSERT(!attached());
    KASSERT(!page_fault_.relation().attached());
    KASSERT(!page_fault_.active());
}

auto Wait::attached() const noexcept -> bool {
    kernel::sync::IrqLockGuard guard{lock_};
    return phase_ != EdgePhase::Detached;
}

auto Wait::ready() const noexcept -> bool {
    return ready_.load<libk::MemoryOrder::Acquire>();
}

auto Wait::observation_key() const noexcept
    -> diag::concurrency::ObservationKey {
    kernel::sync::IrqLockGuard guard{lock_};
    return completion_ == nullptr
        ? diag::concurrency::ObservationKey{}
        : completion_->observation_key();
}

auto Wait::begin(
    Completion& completion,
    CpuRegistry& cpus,
    sched::Binding& binding) noexcept -> bool {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (phase_ != EdgePhase::Detached || completion.attached()) {
            return false;
        }
        completion_ = &completion;
        cpus_ = &cpus;
        binding_ = &binding;
        ready_.store<libk::MemoryOrder::Relaxed>(false);
        completion.attach(*this, binding);
        binding_->link_wait(
            completion.observation_key(),
            diag::concurrency::WaitKind::OperationCompletion,
            diag::concurrency::NodeRef::observation(
                completion.observation_key()));
        phase_ = EdgePhase::Attached;
    }
    return true;
}

auto Wait::finish(arch::TrapContext& trap) noexcept -> bool {
    Completion* completion{};
    CpuRegistry* cpus{};
    sched::Binding* binding{};
    diag::concurrency::ObservationKey operation{};
    bool claimed{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (phase_ != EdgePhase::Attached
            || completion_ == nullptr
            || !ready_.load<libk::MemoryOrder::Acquire>()) {
            return false;
        }
        completion = completion_;
        cpus = cpus_;
        binding = binding_;
        operation = completion->observation_key();
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
            KASSERT(false);
            return false;
        }
    }

    if (!claimed) {
        const auto driver = current_cpu().descriptor == nullptr
            ? diag::concurrency::NodeRef{}
            : diag::concurrency::NodeRef::cpu(
                  current_cpu().descriptor->logical_id());
        auto observation = diag::concurrency::ObservationLease::borrow(
            operation);
        diag::concurrency::CpuWaitScope wait_scope{
            observation,
            diag::concurrency::WaitKind::CompletionPublication,
            diag::concurrency::NodeRef::observation(operation),
            driver};
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
            KASSERT(claim == Completion::FinishClaim::Publishing);
            libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
            wait_scope.observe(static_cast<u64>(claim));
        }
    }

    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(phase_ == EdgePhase::FinishOwned && completion_ == completion);
        completion_ = nullptr;
        cpus_ = nullptr;
        binding_ = nullptr;
        ready_.store<libk::MemoryOrder::Relaxed>(false);
        phase_ = EdgePhase::Detached;
    }
    if (binding != nullptr) {
        binding->clear_wait();
    }
    const Completion::ResumeResult result = completion->finish_claimed(trap);
    if (result == Completion::ResumeResult::Rearm) {
        KASSERT(cpus != nullptr && binding != nullptr);
        KASSERT(begin(*completion, *cpus, *binding));
        if (completion->complete()) {
            completion->signal();
        }
    }
    return true;
}

auto Wait::cancel() noexcept -> bool {
    Completion* completion{};
    sched::Binding* binding{};
    bool was_ready{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (phase_ != EdgePhase::Attached || completion_ == nullptr) {
            return false;
        }
        completion = completion_;
        binding = binding_;
        was_ready = ready_.load<libk::MemoryOrder::Acquire>();
        // Keep the pointer and its exact edge fields in the container while
        // cancellation resolves policy outside the lock.  Delivery now pins
        // the owner against producer publication and finish.
        if (!completion->try_claim_cancel()) {
            return false;
        }
        phase_ = EdgePhase::CancelOwned;
    }

    Completion::CancelResult resolution = completion->resolve_cancel();
    if (resolution == Completion::CancelResult::Reopen) {
        bool reopened{};
        {
            kernel::sync::IrqLockGuard guard{lock_};
            KASSERT(phase_ == EdgePhase::CancelOwned
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
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(phase_ == EdgePhase::CancelOwned
            && completion_ == completion);
        completion_ = nullptr;
        cpus_ = nullptr;
        binding_ = nullptr;
        ready_.store<libk::MemoryOrder::Relaxed>(false);
        phase_ = EdgePhase::Detached;
    }
    if (binding != nullptr) {
        binding->clear_wait();
    }
    completion->finalize_cancel(resolution);
    return true;
}

auto Wait::wake() noexcept -> diag::concurrency::ObservationKey {
    CpuRegistry* cpus{};
    sched::Binding* binding{};
    diag::concurrency::ObservationKey operation{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(phase_ == EdgePhase::Attached
            && completion_ != nullptr && cpus_ != nullptr);
        ready_.store<libk::MemoryOrder::Release>(true);
        cpus = cpus_;
        binding = binding_;
        KASSERT(binding != nullptr);
        operation = completion_->observation_key();
    }
    diag::concurrency::ObservationKey delivery{};
    KASSERT(binding != nullptr && sched::wake(
        *cpus,
        *binding,
        operation,
        &delivery));
    return delivery;
}

} // namespace kernel::operation
