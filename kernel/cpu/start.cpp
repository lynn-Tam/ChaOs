#include <platform/riscv-virt/board.hpp>
#include <cpu/start.hpp>

#include <arch/boot_stack.hpp>
#include <arch/cpu.hpp>
#include <arch/interrupt.hpp>
#include <trap.hpp>
#include <console.hpp>
#include <irq/irq.hpp>
#include <state.hpp>
#include <panic.hpp>
#include <cpu/registry.hpp>
#include <cpu/runtime.hpp>
#include <boot/start.hpp>
#include <utility>
#include <boot/link.hpp>
#include <sched/dispatcher.hpp>
#if TEST_ENABLED
#include <test/boot.hpp>
#endif
#include <task/thread.hpp>

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
    CpuHwId observed_hardware_id) noexcept {
    libk_assert(runtime.owner_registry != nullptr);
    libk_assert(runtime.local.descriptor != nullptr);
    libk_assert(runtime.local.descriptor->hardware_id()
        == observed_hardware_id);
    libk_assert(runtime.local.descriptor->state() == CpuState::Starting);
    libk_assert(!arch::interrupts_enabled());
    libk_assert(arch::set_local_cpu_entry(runtime.local.arch_state));
    libk_assert(arch::install_trap());
    libk_assert(runtime.initial_translation);
    runtime.initial_translation->adopt(runtime.local);
}

void start_secondaries(
    CpuRegistry& cpus,
    const mm::Pmm& pmm) noexcept {
    const CpuId boot = cpus.boot_id();
    if (!arch::secondary_start_available()) {
        for (usize index = 0; index < cpus.count(); ++index) {
            const CpuId id{index};
            const CpuDescriptor* const cpu = cpus.descriptor(id);
            if (id != boot && cpu != nullptr
                && cpu->state() == CpuState::Prepared) {
                libk_assert(cpus.fail_start(id, CpuFailure::HsmUnavailable));
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
        libk_assert(runtime != nullptr);
        libk_assert(runtime->start_context.ready());
        const auto record = pmm.phys(mm::Virt{reinterpret_cast<usize>(&runtime->start_context)},
                                     sizeof(runtime->start_context));
        if (!record) {
            libk_assert(cpus.fail_start(id, failure_from(arch::CpuStartError::InvalidEntryAddress)));
            continue;
        }
        const auto started = arch::start_secondary(cpu->hardware_id(),
            secondary_pages().base().base().raw(), record->raw());
        if (!started) {
            libk_assert(cpus.fail_start(id, failure_from(started.error())));
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

    console::print<
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
    libk_assert(runtime.owner_registry != nullptr);
    libk_assert(runtime.local.current_thread() == &runtime.idle());
    libk_assert(arch::active_stack(runtime.local.arch_state)
        == runtime.idle().home_stack_top());
    libk_assert(runtime.owner_registry->publish_online(runtime));

    if (runtime.local.descriptor->logical_id()
        == runtime.owner_registry->boot_id()) {
        print_snapshot(*runtime.owner_registry);
#if TEST_ENABLED
        test::runtime(runtime);
#endif
        console::print<"runtime: entered\n">();
    }
    sched::yield();
    for (;;) {
        arch::wait_for_interrupt();
    }
}

[[noreturn]] void boot_cpu_continue(
    KernelState& kernel,
    CpuRuntime& runtime) noexcept {
    libk_assert(runtime.local.descriptor->logical_id()
        == kernel.cpus().boot_id());
    install_local_entry(runtime, runtime.local.descriptor->hardware_id());
    virt_irq_start(runtime.local.descriptor->hardware_id().raw);
    console::print<"trap install ok\n">();

    auto allocation = kernel.pmm().allocate_page();
    libk_assert(allocation);
    auto page = std::move(allocation).value();
    auto* const payload =
        reinterpret_cast<volatile uint8_t*>(page.bytes());
    *payload = 0xa5;
    libk_assert(*payload == 0xa5);
    page.reset();
    libk_assert(kernel.pmm().verify_invariants());
    libk_assert(arch_boot_stack_guard_intact());

    start_secondaries(kernel.cpus(), kernel.pmm());
    runtime.dispatcher().enter_idle();
}
extern "C" [[noreturn]] void kernel_secondary_continue(
    CpuRuntime* runtime,
    usize observed_hardware_id) noexcept {
    libk_assert(runtime != nullptr);
    auto& cpu = *runtime;
    install_local_entry(cpu, CpuHwId{observed_hardware_id});
    cpu.dispatcher().enter_idle();
}
