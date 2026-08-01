#include <init/stage_b_probe.hpp>

#if MYOS_CONCURRENCY_PROBE == 13

#include <arch/cpu.hpp>
#include <arch/interrupt.hpp>
#include <arch/riscv64/trap/context.hpp>
#include <core/debug.hpp>
#include <cpu/cpu_local.hpp>
#include <cpu/cpu_registry.hpp>
#include <diag/console.hpp>
#include <diag/concurrency.hpp>
#include <libk/manual_lifetime.hpp>
#include <libk/utility.hpp>
#include <object/object_ref.hpp>
#include <object/object_store.hpp>
#include <operation/completion.hpp>
#include <operation/wait.hpp>
#include <cap/grant_graph.hpp>
#include <cap/rights.hpp>
#include <core/kernel_state.hpp>
#include <init/root_task.hpp>
#include <mm/translation.hpp>
#include <mm/vspace.hpp>
#include <resource/pool.hpp>
#include <sync/completion.hpp>

namespace kernel::init::stage_b {
namespace {

struct Control final {
    libk::Atomic<u32> command{};
    libk::Atomic<u32> claimed{};
    libk::Atomic<u32> done{};
    libk::Atomic<u32> gate{};
    //Confirmatory experiment.
    // Exit condition: remove subject-scoped matching with the Stage B
    // rendezvous once an external scheduler harness can provide these
    // production interleavings without a global probe gate.
    libk::Atomic<u64> subject{};
    libk::Atomic<u32> hit{};
    libk::Atomic<u32> released{};
    libk::Atomic<usize> target{};
    libk::Atomic<u64> ready[CpuSet::word_count]{};
    Action action{};
    void* argument{};
};

constinit Control control{};

constexpr auto gate_raw(Gate gate) noexcept -> u32 {
    return static_cast<u32>(gate);
}

[[nodiscard]] auto current_cpu() noexcept -> CpuId {
    void* const owner = arch::current_cpu_owner();
    KASSERT(owner != nullptr);
    const auto& cpu = *static_cast<const CpuLocal*>(owner);
    KASSERT(cpu.descriptor != nullptr);
    return cpu.descriptor->logical_id();
}

void spin() noexcept {
    __asm__ volatile("nop" ::: "memory");
}

[[nodiscard]] auto pause_at(Gate gate, u64 subject) noexcept -> bool {
    if (control.gate.load<libk::MemoryOrder::Acquire>() != gate_raw(gate)) {
        return false;
    }
    if (control.subject.load<libk::MemoryOrder::Acquire>() != subject) {
        return false;
    }
    control.hit.store<libk::MemoryOrder::Release>(gate_raw(gate));
    while (control.released.load<libk::MemoryOrder::Acquire>()
        != gate_raw(gate)) {
        spin();
    }
    return true;
}

void wait_done() noexcept {
    while (control.done.load<libk::MemoryOrder::Acquire>() == 0) {
        spin();
    }
}

void clear_gate() noexcept {
    control.gate.store<libk::MemoryOrder::Release>(gate_raw(Gate::None));
    control.subject.store<libk::MemoryOrder::Release>(0);
    control.hit.store<libk::MemoryOrder::Release>(gate_raw(Gate::None));
    control.released.store<libk::MemoryOrder::Release>(gate_raw(Gate::None));
}

constexpr usize filler_slots =
    diag::concurrency::ObservationShard::slot_count;
// ManualLifetime keeps the static capacity fixture in .bss without emitting
// a global destructor/fini-array entry into the freestanding kernel image.
libk::ManualLifetime<diag::concurrency::ObservationLease>
    fillers[2][filler_slots]{};
libk::ManualLifetime<cap::GrantGraph> probe_grants{};

void clear_fillers() noexcept {
    for (auto& cpu : fillers) {
        for (auto& lease : cpu) {
            lease.reset();
        }
    }
}

[[nodiscard]] auto fill_cpu(CpuId cpu, usize index) noexcept -> bool {
    KASSERT(index < 2);
    usize filled{};
    for (; filled < filler_slots; ++filled) {
        auto lease = diag::concurrency::ObservationLease::reserve_on(
            cpu,
            diag::concurrency::RecordKind::RemoteDelivery,
            reinterpret_cast<u64>(&fillers[index][filled]),
            filled + 1,
            diag::concurrency::Expectation::InternalFinite);
        if (!lease) {
            break;
        }
        static_cast<void>(fillers[index][filled].emplace(libk::move(lease)));
    }
    // Existing runtime observations already consume part of the shard. The
    // capacity arm succeeds when the first failed reserve proves that this
    // producer has filled every remaining slot, not only when it owns the
    // compile-time slot count exactly.
    return filled < filler_slots;
}

struct CompletionAction final {
    operation::Completion* completion{};
    RootTask* root{};
};

void signal_completion(void* argument) noexcept {
    auto& action = *static_cast<CompletionAction*>(argument);
    KASSERT(action.root != nullptr);
    action.root->stage_b_complete_operation();
    action.completion->signal();
}

struct ShootdownAction final {
    mm::TranslationState state{};
    CpuRegistry* cpus{};
    CpuSet targets{};
    CpuId local{};
    libk::ManualLifetime<mm::ShootdownTicket> ticket{};
    bool committed{};
};

void run_shootdown(void* argument) noexcept {
    auto& action = *static_cast<ShootdownAction*>(argument);
    auto mutation = action.state.begin();
    KASSERT(mutation);
    auto plan = mm::ShootdownPlan::prepare(
        *action.cpus, action.local, action.targets);
    KASSERT(plan);
    const mm::ShootdownStatus status = mutation.value().commit(
        libk::move(plan).value(), *action.ticket);
    action.committed = true;
    while (!action.ticket->complete()) {
        static_cast<void>(mm::retry_shootdowns(
            *action.cpus, *action.ticket));
        __asm__ volatile("nop" ::: "memory");
    }
    static_cast<void>(status);
}

struct GrantAction final {
    cap::GrantGraph* graph{};
};

void service_grants(void* argument) noexcept {
    auto& action = *static_cast<GrantAction*>(argument);
    static_cast<void>(action.graph->service(8));
}

[[nodiscard]] auto empty_grant_notifier() noexcept
    -> diag::concurrency::ObservationKey {
    return {};
}

struct PoolAction final {
    resource::ResourcePool* pool{};
};

void close_pool(void* argument) noexcept {
    auto& action = *static_cast<PoolAction*>(argument);
    static_cast<void>(action.pool->close());
}

struct VSpaceAction final {
    mm::VSpace* space{};
    mm::VmContext context{};
};

void service_vspace(void* argument) noexcept {
    auto& action = *static_cast<VSpaceAction*>(argument);
    const auto result = action.space->service(action.context);
    KASSERT(result);
}

[[nodiscard]] auto worker_cpu(const CpuRegistry& cpus) noexcept -> CpuId {
    KASSERT(cpus.count() >= 4);
    const CpuId boot = cpus.boot_id();
    for (usize raw = 0; raw < cpus.count(); ++raw) {
        const CpuId candidate{raw};
        if (candidate != boot) {
            return candidate;
        }
    }
    KASSERT(false);
    return boot;
}

[[nodiscard]] auto target_cpus(const CpuRegistry& cpus) noexcept -> CpuSet {
    KASSERT(cpus.count() >= 4);
    CpuSet result{};
    const CpuId boot = cpus.boot_id();
    for (usize raw = 0; raw < cpus.count(); ++raw) {
        const CpuId candidate{raw};
        if (candidate != boot) {
            static_cast<void>(result.insert(candidate));
        }
    }
    return result;
}

[[nodiscard]] auto wait_for_online(CpuRegistry& cpus) noexcept -> bool {
    // The boot hart enters the probe immediately after publishing itself;
    // secondary harts can still be in Starting while their idle loops are
    // being installed.  Wait on the registry publication rather than taking
    // one startup-time snapshot, but retain a finite bound for a real start
    // failure.
    constexpr usize spin_limit = 50'000'000;
    for (usize spin_count = 0; spin_count < spin_limit; ++spin_count) {
        const CpuSnapshot snapshot = cpus.snapshot();
        if (snapshot.online >= 4) {
            return snapshot.failed == 0;
        }
        if (snapshot.failed != 0) {
            return false;
        }
        spin();
    }
    return false;
}

[[nodiscard]] auto wait_for_workers(CpuRegistry& cpus) noexcept -> bool {
    constexpr usize spin_limit = 50'000'000;
    for (usize spin_count = 0; spin_count < spin_limit; ++spin_count) {
        bool ready = true;
        for (usize raw = 0; raw < cpus.count(); ++raw) {
            const CpuId id{raw};
            if (id == cpus.boot_id()) {
                continue;
            }
            const u64 word = control.ready[id.raw / CpuSet::word_bits]
                .load<libk::MemoryOrder::Acquire>();
            if ((word & (u64{1} << (id.raw % CpuSet::word_bits))) == 0) {
                ready = false;
                break;
            }
        }
        if (ready) {
            return true;
        }
        spin();
    }
    return false;
}

[[nodiscard]] auto check_key(
    diag::concurrency::ObservationKey key,
    bool capacity_loss) noexcept -> bool {
    return !capacity_loss || !key;
}

[[nodiscard]] auto prepare_fillers(
    CpuRegistry& cpus,
    bool capacity_loss) noexcept -> bool {
    clear_fillers();
    if (!capacity_loss) {
        return true;
    }
    return fill_cpu(cpus.boot_id(), 0)
        && fill_cpu(worker_cpu(cpus), 1);
}

[[nodiscard]] auto finish_fillers() noexcept -> bool {
    clear_fillers();
    return true;
}

[[nodiscard]] auto run_completion(
    CpuRegistry& cpus,
    RootTask& root,
    bool capacity_loss) noexcept -> bool {
    const bool fillers_ok = prepare_fillers(cpus, capacity_loss);
    const bool operation_ok = fillers_ok
        && root.prepare_stage_b_operation(cpus, capacity_loss);
    if (!operation_ok) {
        if (capacity_loss) {
            diag::concurrency::clear_reserve_denial();
        }
        diag::console::print<
            "concurrency-probe: stage-b completion-setup-fail fillers={} "
            "operation={} exhausted={}\n">(
            fillers_ok,
            operation_ok,
            capacity_loss);
        static_cast<void>(finish_fillers());
        return false;
    }
    operation::Completion& completion = root.stage_b_operation();
    operation::Wait& wait = root.stage_b_wait();
    const diag::concurrency::ObservationKey key = completion.observation_key();
    CompletionAction action{&completion, &root};
    dispatch(
        &signal_completion,
        &action,
        worker_cpu(cpus),
        Gate::CompletionReady,
        reinterpret_cast<u64>(&completion));
    // The producer has published canonical Ready but remains paused before
    // its final diagnostic write. The consumer wins the actual operation
    // terminal path from the boot hart through Wait::finish(), not cancel().
    const bool gate = reached(Gate::CompletionReady);
    arch::riscv64::TrapFrame frame{};
    auto trap = arch::riscv64::make_context(frame);
    const bool finished = gate;
    if (finished) {
        wait.finish(trap);
    }
    release(Gate::CompletionReady);
    const bool trap_result = frame.a0
            == static_cast<u64>(static_cast<isize>(MYOS_STATUS_OK))
        && frame.a1 == 0;
    const bool result = finished && !wait.attached() && trap_result
        && completion.complete()
        && root.stage_b_release_count() == 1
        && check_key(key, capacity_loss)
        && !completion.observation_key();
    if (!result) {
        diag::console::print<
            "concurrency-probe: stage-b completion-fail gate={} finished={} "
            "complete={} releases={} key={:#x} final={:#x}\n">(
            gate,
            finished,
            completion.complete(),
            root.stage_b_release_count(),
            key.raw,
            completion.observation_key().raw);
    }
    root.finish_stage_b_operation();
    if (capacity_loss) {
        diag::concurrency::clear_reserve_denial();
    }
    static_cast<void>(finish_fillers());
    return result;
}

[[nodiscard]] auto run_shootdown(
    CpuRegistry& cpus,
    bool capacity_loss) noexcept -> bool {
    if (!prepare_fillers(cpus, capacity_loss)) {
        return false;
    }
    ShootdownAction action{};
    action.cpus = &cpus;
    action.local = worker_cpu(cpus);
    action.targets = target_cpus(cpus);
    action.targets.for_each([&](CpuId cpu) {
        static_cast<void>(action.state.enter(cpu));
    });
    static_cast<void>(action.ticket.emplace());
    if (capacity_loss) {
        //Confirmatory experiment.
        // Exit condition: remove with the external Stage B reserve fault
        // harness once a real shootdown injector can deny this reservation.
        diag::concurrency::deny_reserves(
            diag::concurrency::RecordKind::Shootdown,
            reinterpret_cast<u64>(&*action.ticket));
    }
    dispatch(
        &run_shootdown,
        &action,
        worker_cpu(cpus),
        Gate::ShootdownFinal,
        reinterpret_cast<u64>(&*action.ticket));
    const auto key = action.ticket->observation_key();
    const bool gate = reached(Gate::ShootdownFinal);
    const bool pending = !action.ticket->complete()
        && action.state.pending_tickets() == 1;
    release(Gate::ShootdownFinal);
    const bool result = gate && pending && action.committed
        && action.ticket->complete()
        && action.state.pending_tickets() == 0
        && check_key(key, capacity_loss)
        && !action.ticket->observation_key();
    if (!result) {
        diag::console::print<
            "concurrency-probe: stage-b shootdown-fail gate={} pending={} "
            "committed={} complete={} tickets={} key={:#x} final={:#x}\n">(
            gate,
            pending,
            action.committed,
            action.ticket->complete(),
            action.state.pending_tickets(),
            key.raw,
            action.ticket->observation_key().raw);
    }
    if (capacity_loss) {
        diag::concurrency::clear_reserve_denial();
    }
    action.targets.for_each([&](CpuId cpu) {
        static_cast<void>(action.state.leave(cpu));
    });
    action.ticket.reset();
    static_cast<void>(finish_fillers());
    return result;
}

[[nodiscard]] auto run_grant(
    KernelState& kernel,
    CpuRegistry& cpus,
    bool capacity_loss) noexcept -> bool {
    if (!prepare_fillers(cpus, capacity_loss)) {
        diag::console::print<
            "concurrency-probe: stage-b grant-setup-fail fillers exhausted={}\n">(
            capacity_loss);
        return false;
    }
    auto root_target = kernel.root_pool_ref();
    if (!root_target) {
        diag::console::print<
            "concurrency-probe: stage-b grant-setup-fail root-target exhausted={}\n">(
            capacity_loss);
        static_cast<void>(finish_fillers());
        return false;
    }
    cap::GrantGraph& graph = probe_grants.emplace(kernel.pmm());
    auto root = graph.create_root(
        libk::move(root_target).value(), cap::GrantCeiling{
            .rights = {},
            .data = cap::ResourcePoolAuthority{},
        });
    if (!root) {
        diag::console::print<
            "concurrency-probe: stage-b grant-setup-fail create-root exhausted={}\n">(
            capacity_loss);
        probe_grants.reset();
        static_cast<void>(finish_fillers());
        return false;
    }
    auto source = root.value().acquire();
    if (!source) {
        diag::console::print<
            "concurrency-probe: stage-b grant-setup-fail acquire-source "
            "exhausted={}\n">(capacity_loss);
        root.value().reset();
        probe_grants.reset();
        static_cast<void>(finish_fillers());
        return false;
    }
    auto child_target = source.value().clone_target();
    if (!child_target) {
        diag::console::print<
            "concurrency-probe: stage-b grant-setup-fail clone-target "
            "exhausted={}\n">(capacity_loss);
        root.value().reset();
        source.value().reset();
        probe_grants.reset();
        static_cast<void>(finish_fillers());
        return false;
    }
    auto child = graph.derive(
        source.value(),
        libk::move(child_target).value(),
        cap::GrantCeiling{
            .rights = {},
            .data = cap::ResourcePoolAuthority{},
        });
    if (!child) {
        diag::console::print<
            "concurrency-probe: stage-b grant-setup-fail derive-child "
            "exhausted={}\n">(capacity_loss);
        root.value().reset();
        source.value().reset();
        probe_grants.reset();
        static_cast<void>(finish_fillers());
        return false;
    }
    // The derivation lease is only a construction witness. Release it before
    // invalidation so the child has no live operation and the queued service
    // can perform the terminal acknowledgement.
    source.value().reset();
    cap::GrantRevoke revoke{};
    graph.bind_work_notifier(
        cap::GrantGraph::WorkNotifier{&empty_grant_notifier});
    if (capacity_loss) {
        //Confirmatory experiment.
        // Exit condition: remove with the external Stage B reserve fault
        // harness once a revoke-specific injector can deny this reservation.
        diag::concurrency::deny_reserves(
            diag::concurrency::RecordKind::GrantRevoke,
            reinterpret_cast<u64>(&revoke));
    }
    const auto invalidated = graph.invalidate(root.value().key(), revoke);
    if (!invalidated) {
        if (capacity_loss) {
            diag::concurrency::clear_reserve_denial();
        }
        diag::console::print<
            "concurrency-probe: stage-b grant-setup-fail invalidate exhausted={}\n">(
            capacity_loss);
        graph.unbind_work_notifier();
        child.value().reset();
        root.value().reset();
        source.value().reset();
        probe_grants.reset();
        static_cast<void>(finish_fillers());
        return false;
    }
    GrantAction action{&graph};
    const auto key = revoke.observation_key();
    dispatch(
        &service_grants,
        &action,
        worker_cpu(cpus),
        Gate::GrantFinal,
        reinterpret_cast<u64>(&revoke));
    const bool gate = reached(Gate::GrantFinal);
    const bool pending = !revoke.complete();
    // A second service invocation overlaps the final node acknowledgement
    // while the worker is paused inside the reusable completion callback.
    static_cast<void>(graph.service(8));
    release(Gate::GrantFinal);
    const bool result = gate && pending && revoke.complete()
        && check_key(key, capacity_loss) && !revoke.observation_key();
    if (!result) {
        diag::console::print<
            "concurrency-probe: stage-b grant-fail gate={} pending={} "
            "complete={} key={:#x} final={:#x} exhausted={}\n">(
            gate,
            pending,
            revoke.complete(),
            key.raw,
            revoke.observation_key().raw,
            capacity_loss);
    }
    if (capacity_loss) {
        diag::concurrency::clear_reserve_denial();
    }
    root.value().reset();
    child.value().reset();
    source.value().reset();
    graph.unbind_work_notifier();
    probe_grants.reset();
    static_cast<void>(finish_fillers());
    return result;
}

[[nodiscard]] auto run_pool(
    KernelState& kernel,
    CpuRegistry& cpus,
    bool capacity_loss) noexcept -> bool {
    if (!prepare_fillers(cpus, capacity_loss)) {
        return false;
    }
    auto made = kernel.objects().create_resource(resource::Budget{
        .memory = mm::page_size,
        .caps = 1,
    });
    if (!made) {
        diag::console::print<
            "concurrency-probe: stage-b pool-setup-fail create exhausted={}\n">(
            capacity_loss);
        static_cast<void>(finish_fillers());
        return false;
    }
    auto pool = libk::move(made).value().publish();
    resource::ResourcePool& object = pool.get();
    PoolAction action{&object};
    if (capacity_loss) {
        //Confirmatory experiment.
        // Exit condition: remove with the external Stage B reserve fault
        // harness once a pool-specific injector can deny this reservation.
        diag::concurrency::deny_reserves(
            diag::concurrency::RecordKind::ResourceClose,
            reinterpret_cast<u64>(&object));
    }
    dispatch(
        &close_pool,
        &action,
        worker_cpu(cpus),
        Gate::PoolFirstService,
        reinterpret_cast<u64>(&object));
    const auto key = object.close_observation_key_for_probe();
    const bool first = reached(Gate::PoolFirstService);
    static_cast<void>(object.close());
    const bool reentrant = reached(Gate::PoolReentrant);
    release(Gate::PoolFirstService);
    const bool result = first && reentrant
        && object.state() == resource::PoolState::Closed
        && check_key(key, capacity_loss)
        && !object.close_observation_key_for_probe();
    if (!result) {
        diag::console::print<
            "concurrency-probe: stage-b pool-fail first={} reentrant={} "
            "state={} key={:#x} final={:#x} exhausted={}\n">(
            first,
            reentrant,
            static_cast<u8>(object.state()),
            key.raw,
            object.close_observation_key_for_probe().raw,
            capacity_loss);
    }
    if (capacity_loss) {
        diag::concurrency::clear_reserve_denial();
    }
    pool.reset();
    static_cast<void>(kernel.objects().drain_reclaim());
    static_cast<void>(finish_fillers());
    return result;
}

[[nodiscard]] auto run_vspace(
    KernelState& kernel,
    CpuRegistry& cpus,
    bool capacity_loss) noexcept -> bool {
    if (!prepare_fillers(cpus, capacity_loss)) {
        return false;
    }
    auto made = kernel.objects().create_vspace(kernel.kernel_vspace());
    if (!made) {
        static_cast<void>(finish_fillers());
        return false;
    }
    auto space = libk::move(made).value().publish();
    mm::VSpace& object = space.get();
    const mm::VmContext context{.cpus = &cpus, .local = cpus.boot_id()};
    if (capacity_loss) {
        //Confirmatory experiment.
        // Exit condition: remove with the external Stage B reserve fault
        // harness once VSpace work can be denied for this whole lifecycle.
        diag::concurrency::deny_reserves(
            diag::concurrency::RecordKind::VSpaceWork,
            reinterpret_cast<u64>(&object));
    }
    const auto initial = object.service(context);
    if (!initial || !object.enter_cpu_for_probe(cpus.boot_id())) {
        if (capacity_loss) {
            diag::concurrency::clear_reserve_denial();
        }
        static_cast<void>(finish_fillers());
        return false;
    }
    const bool retired = space.retire();
    if (!retired) {
        if (capacity_loss) {
            diag::concurrency::clear_reserve_denial();
        }
        static_cast<void>(finish_fillers());
        return false;
    }
    VSpaceAction action{&object, context};
    dispatch(
        &service_vspace,
        &action,
        worker_cpu(cpus),
        Gate::None,
        reinterpret_cast<u64>(&object));
    object.leave_cpu_for_probe(cpus.boot_id());
    const auto key = object.observation_key_for_probe();
    dispatch(
        &service_vspace,
        &action,
        worker_cpu(cpus),
        Gate::VSpaceQuiescent,
        reinterpret_cast<u64>(&object));
    const bool gate = reached(Gate::VSpaceQuiescent);
    object.schedule_for_probe();
    release(Gate::VSpaceQuiescent);
    const bool gate_ok = capacity_loss ? !gate : gate;
    const bool result = gate_ok
        && object.state() == mm::VSpaceState::Quiescent
        && check_key(key, capacity_loss)
        && !object.observation_key_for_probe();
    if (!result) {
        diag::console::print<
            "concurrency-probe: stage-b vspace-fail gate={} state={} "
            "key={:#x} final={:#x} exhausted={}\n">(
            gate,
            static_cast<u8>(object.state()),
            key.raw,
            object.observation_key_for_probe().raw,
            capacity_loss);
    }
    if (capacity_loss) {
        diag::concurrency::clear_reserve_denial();
    }
    space.reset();
    static_cast<void>(kernel.objects().drain_reclaim());
    static_cast<void>(finish_fillers());
    return result;
}

[[nodiscard]] auto run_round(
    KernelState& kernel,
    CpuRegistry& cpus,
    RootTask& root,
    bool capacity_loss) noexcept -> bool {
    const bool completion = run_completion(cpus, root, capacity_loss);
    diag::console::print<
        "concurrency-probe: stage-b completion={} exhausted={}\n">(
        completion, capacity_loss);
    if (!completion) {
        return false;
    }
    const bool shootdown = run_shootdown(cpus, capacity_loss);
    diag::console::print<
        "concurrency-probe: stage-b shootdown={} exhausted={}\n">(
        shootdown, capacity_loss);
    if (!shootdown) {
        return false;
    }
    const bool grant = run_grant(kernel, cpus, capacity_loss);
    diag::console::print<
        "concurrency-probe: stage-b grant={} exhausted={}\n">(
        grant, capacity_loss);
    if (!grant) {
        return false;
    }
    const bool pool = run_pool(kernel, cpus, capacity_loss);
    diag::console::print<
        "concurrency-probe: stage-b pool={} exhausted={}\n">(
        pool, capacity_loss);
    if (!pool) {
        return false;
    }
    const bool vspace = run_vspace(kernel, cpus, capacity_loss);
    diag::console::print<
        "concurrency-probe: stage-b vspace={} exhausted={}\n">(
        vspace, capacity_loss);
    return vspace;
}

} // namespace

auto pause(Gate gate, u64 subject) noexcept -> bool {
    return pause_at(gate, subject);
}

void dispatch(
    Action action,
    void* argument,
    CpuId target,
    Gate gate,
    u64 subject) noexcept {
    // CpuId{0} is a valid logical hart, so validity comes from the registry
    // selection performed by the caller rather than the zero value.
    KASSERT(action != nullptr);
    clear_gate();
    control.action = action;
    control.argument = argument;
    control.target.store<libk::MemoryOrder::Release>(target.raw);
    control.claimed.store<libk::MemoryOrder::Release>(0);
    control.done.store<libk::MemoryOrder::Release>(0);
    control.hit.store<libk::MemoryOrder::Release>(gate_raw(Gate::None));
    control.released.store<libk::MemoryOrder::Release>(gate_raw(Gate::None));
    control.subject.store<libk::MemoryOrder::Release>(subject);
    control.gate.store<libk::MemoryOrder::Release>(gate_raw(gate));
    control.command.store<libk::MemoryOrder::Release>(1);
    if (gate == Gate::None) {
        wait_done();
    } else {
        while (control.hit.load<libk::MemoryOrder::Acquire>()
                != gate_raw(gate)
            && control.done.load<libk::MemoryOrder::Acquire>() == 0) {
            spin();
        }
    }
    control.command.store<libk::MemoryOrder::Release>(0);
}

void release(Gate gate) noexcept {
    control.released.store<libk::MemoryOrder::Release>(gate_raw(gate));
    wait_done();
    clear_gate();
}

void join() noexcept {
    wait_done();
}

auto reached(Gate gate) noexcept -> bool {
    return control.hit.load<libk::MemoryOrder::Acquire>() == gate_raw(gate);
}

void mark(Gate gate, u64 subject) noexcept {
    const Gate armed = static_cast<Gate>(
        control.gate.load<libk::MemoryOrder::Acquire>());
    if (control.subject.load<libk::MemoryOrder::Acquire>() != subject) {
        return;
    }
    if (armed == gate
        || (armed == Gate::PoolFirstService
            && gate == Gate::PoolReentrant)) {
        control.hit.store<libk::MemoryOrder::Release>(gate_raw(gate));
    }
}

[[noreturn]] void worker() noexcept {
    const CpuId id = current_cpu();
    // cpu_idle_entry reaches this probe before the regular dispatcher enables
    // interrupts. Keep the hart in the real trap path so shootdown IPIs can
    // drain while the rendezvous worker is spinning.
    arch::enable_interrupts();
    static_cast<void>(control.ready[id.raw / CpuSet::word_bits].fetch_or<
        libk::MemoryOrder::Release>(u64{1} << (id.raw % CpuSet::word_bits)));
    for (;;) {
        if (control.command.load<libk::MemoryOrder::Acquire>() == 0
            || control.target.load<libk::MemoryOrder::Acquire>() != id.raw) {
            spin();
            continue;
        }
        u32 expected = 0;
        if (!control.claimed.compare_exchange_strong<
                libk::MemoryOrder::AcqRel,
                libk::MemoryOrder::Acquire>(expected, 1)) {
            spin();
            continue;
        }
        const Action action = control.action;
        KASSERT(action != nullptr);
        action(control.argument);
        control.done.store<libk::MemoryOrder::Release>(1);
        while (control.command.load<libk::MemoryOrder::Acquire>() != 0) {
            spin();
        }
        control.claimed.store<libk::MemoryOrder::Release>(0);
    }
}

auto run(
    KernelState& kernel,
    CpuRegistry& cpus,
    RootTask& root) noexcept -> bool {
    if (!wait_for_online(cpus)) {
        diag::console::print<
            "concurrency-probe: stage-b ownership-fail=online\n">();
        return false;
    }
    if (!wait_for_workers(cpus)) {
        diag::console::print<
            "concurrency-probe: stage-b ownership-fail=workers\n">();
        return false;
    }
    const bool normal = run_round(kernel, cpus, root, false);
    const bool exhausted = normal && run_round(kernel, cpus, root, true);
    diag::console::print<
        "concurrency-probe: stage-b ownership-{} normal={} exhausted={}\n">(
        normal && exhausted ? "ok" : "fail", normal, exhausted);
    return normal && exhausted;
}

} // namespace kernel::init::stage_b

#endif
