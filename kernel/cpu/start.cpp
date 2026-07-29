#include <cpu/start.hpp>

#include <arch/boot_stack.hpp>
#include <arch/cpu.hpp>
#include <arch/interrupt.hpp>
#include <arch/ipi.hpp>
#include <arch/trap.hpp>
#include <diag/console.hpp>
#include <irq/irq.hpp>
#include <core/kernel_state.hpp>
#include <core/debug.hpp>
#include <cpu/cpu_registry.hpp>
#include <cpu/cpu_runtime.hpp>
#include <init/run.hpp>
#include <libk/utility.hpp>
#include <mm/direct_map.hpp>
#include <arch/cpu.hpp>
#include <sched/dispatcher.hpp>
#include <sync/trace.hpp>
#include <thread/thread.hpp>

namespace kernel {
namespace {

[[nodiscard]] constexpr auto failure_from(arch::CpuStartError error) noexcept
    -> CpuFailure {
    switch (error) {
    case arch::CpuStartError::NotSupported:
        return CpuFailure::HsmUnavailable;
    case arch::CpuStartError::InvalidHardwareId:
        return CpuFailure::InvalidHardwareId;
    case arch::CpuStartError::InvalidEntryAddress:
        return CpuFailure::InvalidEntryAddress;
    case arch::CpuStartError::AlreadyStarted:
        return CpuFailure::AlreadyStarted;
    case arch::CpuStartError::Rejected:
        return CpuFailure::FirmwareRejected;
    }
    __builtin_unreachable();
}

static_assert(failure_from(arch::CpuStartError::NotSupported)
    == CpuFailure::HsmUnavailable);
static_assert(failure_from(arch::CpuStartError::InvalidHardwareId)
    == CpuFailure::InvalidHardwareId);
static_assert(failure_from(arch::CpuStartError::InvalidEntryAddress)
    == CpuFailure::InvalidEntryAddress);
static_assert(failure_from(arch::CpuStartError::AlreadyStarted)
    == CpuFailure::AlreadyStarted);
static_assert(failure_from(arch::CpuStartError::Rejected)
    == CpuFailure::FirmwareRejected);

void install_local_entry(
    CpuRuntime& runtime,
    CpuHardwareId observed_hardware_id) noexcept {
    KASSERT(runtime.owner_registry != nullptr);
    KASSERT(runtime.local.descriptor != nullptr);
    KASSERT(runtime.local.descriptor->hardware_id()
        == observed_hardware_id);
    KASSERT(runtime.local.descriptor->state() == CpuState::Starting);
    KASSERT(!arch::interrupts_enabled());
    KASSERT(arch::set_local_cpu_entry(runtime.local.arch_state));
    KASSERT(arch::install_trap());
    KASSERT(runtime.initial_translation);
    runtime.initial_translation->adopt(runtime.local);
}

void start_secondaries(
    CpuRegistry& cpus,
    const kernel::mm::DirectMap& direct_map) noexcept {
    const CpuId boot = cpus.boot_id();
    if (!arch::secondary_start_available()) {
        for (usize index = 0; index < cpus.count(); ++index) {
            const CpuId id{index};
            const CpuDescriptor* const cpu = cpus.descriptor(id);
            if (id != boot && cpu != nullptr
                && cpu->state() == CpuState::Prepared) {
                KASSERT(cpus.fail_start(id, CpuFailure::HsmUnavailable));
            }
        }
        return;
    }

    for (usize index = 0; index < cpus.count(); ++index) {
        const CpuId id{index};
        if (id == boot) {
            continue;
        }
        const CpuDescriptor* const cpu = cpus.descriptor(id);
        if (cpu == nullptr || cpu->state() != CpuState::Prepared) {
            continue;
        }
        if (!cpus.begin_start(id)) {
            continue;
        }

        CpuRuntime* const runtime = cpus.runtime(id);
        KASSERT(runtime != nullptr);
        const auto started = arch::start_secondary(
            cpu->hardware_id(), runtime->start_context, direct_map);
        if (!started) {
            KASSERT(cpus.fail_start(id, failure_from(started.error())));
        }
    }
}

void print_snapshot(CpuRegistry& cpus) noexcept {
    // This bounded window is diagnostic only. A hart accepted by firmware may
    // honestly remain Starting after the observation ends.
    constexpr usize observation_scans = 1'000'000;
    CpuSnapshot snapshot{};
    for (usize scan = 0; scan < observation_scans; ++scan) {
        snapshot = cpus.snapshot();
        if (snapshot.starting == 0) {
            break;
        }
    }

    diag::console::print<
        "cpu: discovered={} prepared={} starting={} online={} failed={}\n">(
        cpus.count(),
        snapshot.prepared,
        snapshot.starting,
        snapshot.online,
        snapshot.failed);
}

} // namespace

[[noreturn]] void cpu_idle_entry(void* argument) noexcept {
    auto& runtime = *static_cast<CpuRuntime*>(argument);
    KASSERT(runtime.owner_registry != nullptr);
    KASSERT(runtime.local.current_thread() == &runtime.idle());
    KASSERT(arch::active_stack(runtime.local.arch_state)
        == runtime.idle().home_stack_top());
    KASSERT(runtime.owner_registry->publish_online(runtime));

#if MYOS_LOCK_PROBE
    //Confirmatory experiment.
    // Exit condition: each fatal remains reproducible through run-lock-probe;
    // the whole call is absent from normal builds.
    sync::run_probe(MYOS_LOCK_PROBE);
#endif
#if MYOS_CONCURRENCY_PROBE
    //Confirmatory experiment.
    // Exit condition: remove the in-kernel rendezvous when an external
    // scheduler/fault harness can force the same slot interleavings.
    diag::concurrency::run_probe(MYOS_CONCURRENCY_PROBE);
#endif

    if (runtime.local.descriptor->logical_id()
        == runtime.owner_registry->boot_id()) {
#if MYOS_CONCURRENCY_PROBE == 3 || MYOS_CONCURRENCY_PROBE == 4
        //Confirmatory experiment.
        // Exit condition: remove with the Stage B operation fault probes.
        KASSERT(init::run_concurrency_probe(
            *runtime.owner_registry, MYOS_CONCURRENCY_PROBE));
#endif
        print_snapshot(*runtime.owner_registry);
        if constexpr (sync::lock_trace) {
            sync::dump_diagnostics();
        }
        diag::console::print<"runtime: entered\n">();
#if MYOS_CONCURRENCY_PROBE == 3 || MYOS_CONCURRENCY_PROBE == 4
        //Confirmatory experiment.
        // Exit condition: remove with the Stage B operation fault probes.
        // Their synthetic Wait edge is intentionally non-resumable, so keep
        // the root task out of normal execution for the observation window.
        static_cast<void>(arch::disable_interrupts());
        for (;;) {
            arch::wait_for_interrupt();
        }
#endif
#if MYOS_PANIC_PROBE
        //Confirmatory experiment.
        // Exit condition: remove once panic-owner/peer-stop behavior is
        // exercised by an external QEMU diagnostics harness.
#if MYOS_PANIC_PROBE == 2
        arch::inject_ipi_failures_for_test(max_cpu_count);
#endif
        KASSERT(false);
#endif
    }
    sched::yield();
    for (;;) {
        arch::wait_for_interrupt();
    }
}

[[noreturn]] void boot_cpu_continue(
    KernelState& kernel,
    CpuRuntime& runtime) noexcept {
    KASSERT(runtime.local.descriptor->logical_id()
        == kernel.cpus().boot_id());
    install_local_entry(runtime, runtime.local.descriptor->hardware_id());
    kernel::irq::initialize_platform();
    diag::console::print<"trap install ok\n">();

    auto allocation = kernel.pmm().allocate_page();
    KASSERT(allocation);
    auto page = libk::move(allocation).value();
    auto* const payload =
        reinterpret_cast<volatile uint8_t*>(page.bytes());
    *payload = 0xa5;
    KASSERT(*payload == 0xa5);
    page.reset();
    KASSERT(kernel.pmm().verify_invariants());
    KASSERT(arch_boot_stack_guard_intact());

#if MYOS_CONCURRENCY_PROBE == 2
    //Confirmatory experiment.
    // Exit condition: remove when the normal CPU-offline path is available to
    // the external concurrency scenario harness.
    bool storage_ok{};
    for (usize index = 0; index < kernel.cpus().count(); ++index) {
        const CpuId id{index};
        if (id == kernel.cpus().boot_id()) {
            continue;
        }
        auto lease = diag::concurrency::ObservationLease::reserve_on(
            id,
            diag::concurrency::RecordKind::ExecutionActor,
            0x5a5a,
            1,
            diag::concurrency::Expectation::SchedulerControlled);
        if (!lease) {
            break;
        }
        const auto key = lease.detach_key();
        if (!kernel.cpus().drop_runtime_for_probe(id)) {
            break;
        }
        auto surviving =
            diag::concurrency::ObservationLease::borrow(key);
        diag::concurrency::ObservationSnapshot snapshot{};
        storage_ok = surviving.snapshot(snapshot)
            && snapshot.subject_identity == 0x5a5a;
        surviving.finish(1);
        break;
    }
    diag::console::print<
        "concurrency-probe: stage-a storage-{} cpus={}\n">(
        storage_ok ? "ok" : "fail", kernel.cpus().count());
#endif

    start_secondaries(kernel.cpus(), kernel.direct_map());
    runtime.dispatcher().enter_idle();
}
extern "C" [[noreturn]] void kernel_secondary_continue(
    CpuRuntime* runtime,
    usize observed_hardware_id) noexcept {
    KASSERT(runtime != nullptr);
    auto& cpu = *runtime;
    install_local_entry(cpu, CpuHardwareId{observed_hardware_id});
    cpu.dispatcher().enter_idle();
}

} // namespace kernel
