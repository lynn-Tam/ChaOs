#include <operation/completion.hpp>

#include <core/debug.hpp>
#include <cpu/cpu_local.hpp>
#include <cpu/cpu_registry.hpp>
#include <sched/binding.hpp>
#include <sched/dispatcher.hpp>
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

constexpr u32 operation_attached = 1;
constexpr u32 operation_claimed = 2;
constexpr u32 operation_ready = 3;
constexpr u32 operation_finished = 4;
constexpr u32 operation_cancelled = 5;

} // namespace

Completion::~Completion() noexcept {
    KASSERT(owner_ != nullptr && ops_ != nullptr);
    KASSERT(!attached());
}

void Completion::attach(Wait& wait) noexcept {
    KASSERT(!attached());
    // Wait is the sole caller and publishes its completion_ edge while
    // holding Wait::lock_. Do not call back into Wait here: attached() takes
    // that same non-recursive lock and would self-deadlock the admission path.
    sink_.template emplace<BlockingSink>(BlockingSink{&wait});
    const auto driver = wait.binding_ != nullptr
        ? wait.binding_->actor_ref()
        : current_cpu_node();
    observation_ = diag::concurrency::ObservationLease::reserve(
        diag::concurrency::RecordKind::Operation,
        reinterpret_cast<u64>(owner_),
        1,
        diag::concurrency::Expectation::InternalFinite);
    observation_.transition(
        operation_attached,
        static_cast<u64>(operation_attached),
        diag::concurrency::WaitKind::None,
        driver);
    observation_.watch(true);
    delivery_.store<libk::MemoryOrder::Release>(Delivery::Attached);
}

void Completion::attach(
    Vproc& vproc,
    CpuRegistry& cpus,
    operation::Key key) noexcept {
    KASSERT(!attached() && key.valid());
    KASSERT(vproc.binding() != nullptr);
    sink_.template emplace<VprocSink>(VprocSink{&vproc, &cpus, key});
    const auto driver = vproc.binding() != nullptr
        ? vproc.binding()->actor_ref()
        : current_cpu_node();
    observation_ = diag::concurrency::ObservationLease::reserve(
        diag::concurrency::RecordKind::Operation,
        reinterpret_cast<u64>(owner_),
        key.generation(),
        diag::concurrency::Expectation::ExternalUnbounded);
    observation_.transition(
        operation_attached,
        static_cast<u64>(operation_attached),
        diag::concurrency::WaitKind::None,
        driver);
    delivery_.store<libk::MemoryOrder::Release>(Delivery::Attached);
}

void Completion::signal() noexcept {
    Delivery expected = Delivery::Attached;
    if (!delivery_.compare_exchange_strong<
            libk::MemoryOrder::AcqRel,
            libk::MemoryOrder::Acquire>(expected, Delivery::Claimed)) {
        KASSERT(expected == Delivery::Claimed
            || expected == Delivery::Ready
            || expected == Delivery::Detached);
        return;
    }
    const auto cpu = current_cpu_node();
    observation_.transition(
        operation_claimed,
        static_cast<u64>(operation_claimed),
        diag::concurrency::WaitKind::CompletionPublication,
        cpu);
    diag::concurrency::record(
        diag::concurrency::FlightDomain::Operation,
        diag::concurrency::FlightEvent::OperationClaimed,
        cpu.identity,
        observation_.key().raw);
    // Claim the delivery edge before touching the operation owner. A cancel
    // may otherwise detach and reclaim the owner after publication was
    // decided but before this producer reaches signal().
    KASSERT(complete());
    if (auto* const blocking = libk::get_if<BlockingSink>(&sink_)) {
        KASSERT(blocking->wait != nullptr);
        blocking->wait->wake();
        observation_.transition(
            operation_ready,
            static_cast<u64>(operation_ready),
            diag::concurrency::WaitKind::None,
            cpu);
        diag::concurrency::record(
            diag::concurrency::FlightDomain::Operation,
            diag::concurrency::FlightEvent::OperationReady,
            cpu.identity,
            observation_.key().raw);
        delivery_.store<libk::MemoryOrder::Release>(Delivery::Ready);
        return;
    }
    const VprocSink target = *libk::get_if<VprocSink>(&sink_);
    KASSERT(target.vproc != nullptr && target.cpus != nullptr);
    const Result result = ops_->read(owner_);
    target.vproc->publish_operation(target.key, result, *target.cpus);
    sink_.template emplace<libk::monostate>();
    delivery_.store<libk::MemoryOrder::Release>(Delivery::Detached);
    const u64 observation = observation_.key().raw;
    diag::concurrency::record(
        diag::concurrency::FlightDomain::Operation,
        diag::concurrency::FlightEvent::OperationRelease,
        cpu.identity,
        observation);
    ops_->release(owner_);
    observation_.finish(operation_finished, static_cast<u64>(result.status));
    diag::concurrency::record(
        diag::concurrency::FlightDomain::Operation,
        diag::concurrency::FlightEvent::OperationFinish,
        cpu.identity,
        observation);
}

void Completion::finish(arch::TrapContext& trap) noexcept {
    KASSERT(complete());
    // Blocking publication deliberately wakes the scheduler while retaining
    // Claimed: the producer must keep the operation alive until it has stopped
    // touching the Wait and its Binding.  The resumed owner can observe the
    // Wait's ready bit in that narrow interval, so wait for the producer's
    // final release publication before claiming the result.
    diag::concurrency::CpuWaitScope wait_scope{
        observation_,
        diag::concurrency::WaitKind::CompletionPublication,
        diag::concurrency::NodeRef::observation(observation_.key()),
        current_cpu_node(),
        diag::concurrency::Expectation::InternalFinite};
    Delivery expected = delivery_.load<libk::MemoryOrder::Acquire>();
    while (expected == Delivery::Claimed) {
        libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
        wait_scope.observe(static_cast<u64>(expected));
        expected = delivery_.load<libk::MemoryOrder::Acquire>();
    }
    KASSERT(expected == Delivery::Ready);
    KASSERT((delivery_.compare_exchange_strong<
        libk::MemoryOrder::AcqRel,
        libk::MemoryOrder::Acquire>(expected, Delivery::Claimed)));
    detach();
    if (ops_->resume != nullptr) {
        ops_->resume(owner_, trap);
        const u64 observation = observation_.key().raw;
        diag::concurrency::record(
            diag::concurrency::FlightDomain::Operation,
            diag::concurrency::FlightEvent::OperationRelease,
            current_cpu_node().identity,
            observation);
        ops_->release(owner_);
        observation_.finish(operation_finished);
        diag::concurrency::record(
            diag::concurrency::FlightDomain::Operation,
            diag::concurrency::FlightEvent::OperationFinish,
            current_cpu_node().identity,
            observation);
        return;
    }
    const Result result = ops_->read(owner_);
    trap.set_result(
        0, static_cast<usize>(static_cast<isize>(result.status)));
    trap.set_result(1, result.value);
    const u64 observation = observation_.key().raw;
    diag::concurrency::record(
        diag::concurrency::FlightDomain::Operation,
        diag::concurrency::FlightEvent::OperationRelease,
        current_cpu_node().identity,
        observation);
    ops_->release(owner_);
    observation_.finish(operation_finished, static_cast<u64>(result.status));
    diag::concurrency::record(
        diag::concurrency::FlightDomain::Operation,
        diag::concurrency::FlightEvent::OperationFinish,
        current_cpu_node().identity,
        observation);
}

auto Completion::cancel() noexcept -> bool {
    Delivery observed = delivery_.load<libk::MemoryOrder::Acquire>();
    for (;;) {
        if (observed == Delivery::Detached
            || observed == Delivery::Claimed) {
            return false;
        }
        KASSERT(observed == Delivery::Attached
            || observed == Delivery::Ready);
        if (delivery_.compare_exchange_weak<
                libk::MemoryOrder::AcqRel,
                libk::MemoryOrder::Acquire>(observed, Delivery::Claimed)) {
            break;
        }
    }

    bool drain = observed == Delivery::Ready || complete();
    if (!drain && !ops_->cancel(owner_)) {
        drain = complete();
        if (!drain) {
            delivery_.store<libk::MemoryOrder::Release>(Delivery::Attached);
            return false;
        }
    }
    if (drain) {
        static_cast<void>(ops_->read(owner_));
    }
    sink_.template emplace<libk::monostate>();
    delivery_.store<libk::MemoryOrder::Release>(Delivery::Detached);
    const u64 observation = observation_.key().raw;
    diag::concurrency::record(
        diag::concurrency::FlightDomain::Operation,
        diag::concurrency::FlightEvent::OperationRelease,
        current_cpu_node().identity,
        observation);
    ops_->release(owner_);
    observation_.finish(operation_cancelled);
    diag::concurrency::record(
        diag::concurrency::FlightDomain::Operation,
        diag::concurrency::FlightEvent::OperationCancel,
        current_cpu_node().identity,
        observation);
    return true;
}

void Completion::detach() noexcept {
    KASSERT(delivery_.load<libk::MemoryOrder::Acquire>()
        == Delivery::Claimed);
    sink_.template emplace<libk::monostate>();
    delivery_.store<libk::MemoryOrder::Release>(Delivery::Detached);
}

} // namespace kernel::operation
