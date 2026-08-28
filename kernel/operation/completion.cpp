#include <operation/completion.hpp>

#include <core/debug.hpp>
#include <cpu/cpu_local.hpp>
#include <cpu/cpu_registry.hpp>
#include <sched/binding.hpp>
#include <sched/dispatcher.hpp>
#include <sched/guard.hpp>
#include <execution/vproc.hpp>
#include <operation/wait.hpp>

namespace kernel::operation {
namespace {

[[nodiscard]] auto current_cpu_node() noexcept
    -> diag::concurrency::NodeRef {
    void* const owner = arch::current_cpu_owner();
    if (owner == nullptr) {
        return {};
    }
    const auto& cpu = *static_cast<const CpuLocal*>(owner);
    return cpu.descriptor == nullptr
        ? diag::concurrency::NodeRef{}
        : diag::concurrency::NodeRef::cpu(cpu.descriptor->logical_id());
}

constexpr u32 operation_finished = static_cast<u32>(
    diag::concurrency::OperationPhase::Finished);
constexpr u32 operation_cancelled = static_cast<u32>(
    diag::concurrency::OperationPhase::Cancelled);

} // namespace

Completion::~Completion() noexcept {
    KASSERT(owner_ != nullptr && ops_ != nullptr);
    KASSERT(!attached());
}

void Completion::attach(Wait& wait, sched::Binding& binding) noexcept {
    KASSERT(!attached());
    // Re-emplacement of the owning operation may reuse storage whose previous
    // diagnostic generation has already retired. Clear the publication slot
    // before attempting this generation's optional reservation.
    observation_key_.store<libk::MemoryOrder::Relaxed>(0);
    // Wait is the sole caller and publishes its completion_ edge while
    // holding Wait::lock_. Do not call back into Wait here: attached() takes
    // that same non-recursive lock and would self-deadlock the admission path.
    sink_.template emplace<BlockingSink>(BlockingSink{&wait, &binding});
    // The waiter is a consumer edge.  It cannot also be the operation's
    // producer without manufacturing an Execution -> Operation -> Execution
    // cycle. The configured producer/service edge is replaced by the
    // producer CPU once delivery is claimed.
    const auto driver = policy_.driver
        ? policy_.driver
        : diag::concurrency::NodeRef::external(
              reinterpret_cast<u64>(owner_), 1);
    const auto attached_driver =
        policy_.expectation == diag::concurrency::Expectation::DeadlineBound
            ? policy_.deadline_driver : driver;
    KASSERT(attached_driver);
    if (policy_.grace == 0) {
        policy_.grace =
            diag::concurrency::default_grace(policy_.expectation);
    }
    auto observation = diag::concurrency::ObservationLease::reserve(
        diag::concurrency::RecordKind::Operation,
        reinterpret_cast<u64>(owner_),
        1,
        policy_.expectation);
    observation.set_policy(policy_);
    observation.publish(
        diag::concurrency::OperationPhase::Attached, attached_driver);
    observation_key_.store<libk::MemoryOrder::Release>(
        observation.detach_key().raw);
    delivery_.store<libk::MemoryOrder::Release>(Delivery::Attached);
}

void Completion::attach(
    Vproc& vproc,
    CpuRegistry& cpus,
    operation::Key key) noexcept {
    KASSERT(!attached() && key.valid());
    KASSERT(vproc.binding() != nullptr);
    observation_key_.store<libk::MemoryOrder::Relaxed>(0);
    sink_.template emplace<VprocSink>(VprocSink{&vproc, &cpus, key});
    const auto driver = policy_.driver
        ? policy_.driver
        : diag::concurrency::NodeRef::external(
              reinterpret_cast<u64>(owner_), key.generation());
    const auto attached_driver =
        policy_.expectation == diag::concurrency::Expectation::DeadlineBound
            ? policy_.deadline_driver : driver;
    KASSERT(attached_driver);
    if (policy_.grace == 0) {
        policy_.grace =
            diag::concurrency::default_grace(policy_.expectation);
    }
    auto observation = diag::concurrency::ObservationLease::reserve(
        diag::concurrency::RecordKind::Operation,
        reinterpret_cast<u64>(owner_),
        key.generation(),
        policy_.expectation);
    observation.set_policy(policy_);
    observation.publish(
        diag::concurrency::OperationPhase::Attached, attached_driver);
    observation_key_.store<libk::MemoryOrder::Release>(
        observation.detach_key().raw);
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
        KASSERT(expected == Delivery::Claimed
            || expected == Delivery::Ready
            || expected == Delivery::Detached
            || expected == Delivery::CancelRaced);
        return;
    }
    const auto cpu = current_cpu_node();
    const auto key = observation_key();
    auto observation = diag::concurrency::ObservationLease::borrow(key);
    observation.publish(diag::concurrency::OperationPhase::Claimed, cpu);
    diag::concurrency::record(
        diag::concurrency::FlightDomain::Operation,
        diag::concurrency::FlightEvent::OperationClaimed,
        cpu.identity,
        key.raw);
    // The delivery claim keeps the operation owner and sink live while this
    // producer prepares Ready.  A consumer may clear the sink and release the
    // owner immediately after Ready, so every sink-derived diagnostic value
    // must be captured before that publication.
    KASSERT(complete());
    if (auto* const blocking = libk::get_if<BlockingSink>(&sink_)) {
        KASSERT(blocking->wait != nullptr);
        Wait* const wait = blocking->wait;
        const auto driver = blocking->binding != nullptr
            ? blocking->binding->actor_ref()
            : diag::concurrency::NodeRef{};
        // Wake-before-Ready is the established operation protocol.  Pin the
        // producer on its current CPU only across the interval in which the
        // waiter becomes runnable and the canonical Delivery state is
        // release-published.  Interrupt delivery remains enabled; the guard
        // only defers a local dispatcher switch until the producer has ceased
        // touching the Wait, Binding and sink-derived values.
        sched::PreemptGuard preempt{};
        const auto delivery = wait->wake();
        const auto delivery_node = delivery
            ? diag::concurrency::NodeRef::observation(delivery)
            : cpu;
        observation.publish(
            diag::concurrency::OperationPhase::WakeIssued,
            delivery_node);
        // Delivery is the canonical Completion state.  Only after this
        // release may the diagnostic projection claim that readiness was
        // published.  No sink, binding or owner-derived value is read after
        // this store: Wait::finish/cancel may release them concurrently.
        delivery_.store<libk::MemoryOrder::Release>(Delivery::Ready);
        observation.publish(
            diag::concurrency::OperationPhase::ReadyPublished,
            driver,
            delivery
                ? diag::concurrency::NodeRef::observation(delivery)
                : diag::concurrency::NodeRef{});
        diag::concurrency::record(
            diag::concurrency::FlightDomain::Operation,
            diag::concurrency::FlightEvent::OperationReady,
            cpu.identity,
            key.raw);
        return;
    }
    const VprocSink target = *libk::get_if<VprocSink>(&sink_);
    KASSERT(target.vproc != nullptr && target.cpus != nullptr);
    // Retire the old diagnostic generation and snapshot every member needed
    // by the terminal path before invoking owner/publish callbacks.  Delivery
    // remains Claimed until the sink is cleared, so no new generation can
    // attach while these locals are being consumed.
    const Ops* const ops = ops_;
    void* const owner = owner_;
    const auto terminal = diag::concurrency::ObservationKey{
        observation_key_.exchange<libk::MemoryOrder::AcqRel>(0)};
    const Result result = ops->read(owner);
    target.vproc->publish_operation(target.key, result, *target.cpus);
    sink_.template emplace<libk::monostate>();
    // This is the final Completion member access in the Vproc terminal path.
    delivery_.store<libk::MemoryOrder::Release>(Delivery::Detached);
    // Release may destroy the embedded Completion; all following diagnostics
    // use only captured locals.
    ops->release(owner);
    diag::concurrency::record(
        diag::concurrency::FlightDomain::Operation,
        diag::concurrency::FlightEvent::OperationRelease,
        cpu.identity,
        terminal.raw);
    auto finished = diag::concurrency::ObservationLease::borrow(terminal);
    finished.finish(operation_finished, static_cast<u64>(result.status));
    diag::concurrency::record(
        diag::concurrency::FlightDomain::Operation,
        diag::concurrency::FlightEvent::OperationFinish,
        cpu.identity,
        terminal.raw);
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

auto Completion::try_claim_cancel() noexcept -> bool {
    const Delivery observed = delivery_.load<libk::MemoryOrder::Acquire>();
    if (observed != Delivery::Attached && observed != Delivery::Ready) {
        return false;
    }
    Delivery expected = observed;
    return delivery_.compare_exchange_strong<
        libk::MemoryOrder::AcqRel,
        libk::MemoryOrder::Acquire>(expected, Delivery::Cancelling);
}

auto Completion::resolve_cancel() noexcept -> CancelResult {
    if (complete()) {
        return CancelResult::Completed;
    }
    if (ops_->cancel(owner_)) {
        return CancelResult::Canceled;
    }
    return complete() ? CancelResult::Completed : CancelResult::Reopen;
}

auto Completion::try_reopen_cancel() noexcept -> bool {
    Delivery expected = Delivery::Cancelling;
    const bool reopened = delivery_.compare_exchange_strong<
        libk::MemoryOrder::AcqRel,
        libk::MemoryOrder::Acquire>(expected, Delivery::Attached);
    if (!reopened) {
        KASSERT(expected == Delivery::CancelRaced);
    }
    return reopened;
}

/*luna change: let finish return a closed rearm result, reason: Wait must reattach the same Completion only after the old generation is fully retired*/
auto Completion::finish_claimed(arch::TrapContext& trap) noexcept
    -> ResumeResult {
    KASSERT(complete());
    KASSERT(delivery_.load<libk::MemoryOrder::Acquire>()
        == Delivery::Claimed);
    // Snapshot the immutable callback table, owner and old diagnostic key
    // while the Claimed state still excludes reattachment.  The callbacks
    // then finish the old generation before its sink is cleared.
    const Ops* const ops = ops_;
    void* const owner = owner_;
    const auto terminal = diag::concurrency::ObservationKey{
        observation_key_.exchange<libk::MemoryOrder::AcqRel>(0)};
    const u64 cpu = current_cpu_node().identity;
    if (ops->resume != nullptr) {
        const ResumeResult resume = ops->resume(owner, trap);
        sink_.template emplace<libk::monostate>();
        // The final Completion member access precedes the captured release.
        delivery_.store<libk::MemoryOrder::Release>(Delivery::Detached);
        if (resume == ResumeResult::Done) {
            // Release may destroy the embedded Completion.  No Completion
            // member is accessed after this callback.
            ops->release(owner);
            /*luna change: publish OperationRelease only with terminal owner release, reason: Rearm retires diagnostics but keeps the same operation owner alive*/
            diag::concurrency::record(
                diag::concurrency::FlightDomain::Operation,
                diag::concurrency::FlightEvent::OperationRelease,
                cpu,
                terminal.raw);
        }
        auto finished = diag::concurrency::ObservationLease::borrow(terminal);
        finished.finish(operation_finished);
        diag::concurrency::record(
            diag::concurrency::FlightDomain::Operation,
            diag::concurrency::FlightEvent::OperationFinish,
            cpu,
            terminal.raw);
        return resume;
    }
    const Result result = ops->read(owner);
    trap.set_result(
        0, static_cast<usize>(static_cast<isize>(result.status)));
    trap.set_result(1, result.value);
    sink_.template emplace<libk::monostate>();
    // The release store is the final Completion member access in this path.
    delivery_.store<libk::MemoryOrder::Release>(Delivery::Detached);
    // Release may destroy the embedded Completion; diagnostics below use only
    // captured locals.
    ops->release(owner);
    diag::concurrency::record(
        diag::concurrency::FlightDomain::Operation,
        diag::concurrency::FlightEvent::OperationRelease,
        cpu,
        terminal.raw);
    auto finished = diag::concurrency::ObservationLease::borrow(terminal);
    finished.finish(operation_finished, static_cast<u64>(result.status));
    diag::concurrency::record(
        diag::concurrency::FlightDomain::Operation,
        diag::concurrency::FlightEvent::OperationFinish,
        cpu,
        terminal.raw);
    return ResumeResult::Done;
}

void Completion::finalize_cancel(CancelResult result) noexcept {
    KASSERT(result != CancelResult::Reopen);
    KASSERT(delivery_.load<libk::MemoryOrder::Acquire>()
        == Delivery::Cancelling
        || delivery_.load<libk::MemoryOrder::Acquire>()
            == Delivery::CancelRaced);
    // Cancellation owns the terminal state, so capture the callback table,
    // owner and old diagnostic key before any owner callback.  No new attach
    // can pass while Delivery remains Cancelling/CancelRaced.
    const Ops* const ops = ops_;
    void* const owner = owner_;
    const auto terminal = diag::concurrency::ObservationKey{
        observation_key_.exchange<libk::MemoryOrder::AcqRel>(0)};
    const u64 cpu = current_cpu_node().identity;
    if (result == CancelResult::Completed) {
        static_cast<void>(ops->read(owner));
    }
    sink_.template emplace<libk::monostate>();
    // The release store is the final Completion member access in this path.
    delivery_.store<libk::MemoryOrder::Release>(Delivery::Detached);
    // Release may destroy the embedded Completion; diagnostics below use only
    // captured locals.
    ops->release(owner);
    diag::concurrency::record(
        diag::concurrency::FlightDomain::Operation,
        diag::concurrency::FlightEvent::OperationRelease,
        cpu,
        terminal.raw);
    auto finished = diag::concurrency::ObservationLease::borrow(terminal);
    finished.finish(operation_cancelled);
    diag::concurrency::record(
        diag::concurrency::FlightDomain::Operation,
        diag::concurrency::FlightEvent::OperationCancel,
        cpu,
        terminal.raw);
}

} // namespace kernel::operation
