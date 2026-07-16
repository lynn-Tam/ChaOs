#include <mm/virtual_layout.hpp>
#include <arch/boot_stack.hpp>
#include <arch/cpu.hpp>
#include <boot/cpu_topology.hpp>
#include <boot/run.hpp>
#include <boot/timebase.hpp>
#include <boot/firmware/devicetree/fdt.hpp>
#include <diag/console.hpp>
#include <boot/boot_info.hpp>
#include <core/kernel_state.hpp>
#include <core/debug.hpp>
#include <cpu/cpu_provisioner.hpp>
#include <cpu/cpu_registry.hpp>
#include <cpu/cpu_runtime.hpp>
#include <cpu/start.hpp>
#include <libk/manual_lifetime.hpp>
#include <libk/utility.hpp>
#if MYOS_BUILTIN_TESTS
#include <test/test.hpp>
#endif

namespace {
constinit libk::ManualLifetime<kernel::KernelState> kernel_storage{};
} // namespace

namespace kernel::boot {

[[noreturn]] void run(BootInfo&& boot_info, CpuHardwareId boot_cpu) noexcept {
    KASSERT(arch_boot_stack_guard_intact());
#if MYOS_BUILTIN_TESTS
    const TestStats tests = run_builtin_tests(boot_info);
    KASSERT(arch_boot_stack_guard_intact());
    KASSERT(tests.failed == 0);
#endif

    const auto initialized = kernel::KernelState::initialize_in(
        kernel_storage,
        libk::move(boot_info.memory_regions),
        kernel::mm::DirectMapLayout{
            .physical_base = kernel::mm::PhysAddr{0},
            .virtual_base = kernel::mm::VirtAddr{kernel::mm::layout::DirectMapBegin},
            .window_size = kernel::mm::layout::DirectMapSize,
        });
    KASSERT(initialized);
    kernel::KernelState& kernel = *kernel_storage;

    KASSERT(boot_info.fdt);
    KASSERT(boot_info.transition);
    auto fdt_reservation =
        kernel.pmm().take_boot_reservation_for(boot_info.fdt.pages);
    KASSERT(fdt_reservation);
    auto transition_reservation =
        kernel.pmm().take_boot_reservation_for(boot_info.transition.pages);
    KASSERT(transition_reservation);
    while (auto reservation = kernel.pmm().take_boot_reservation()) {
        const auto reclaimed = kernel.pmm().reclaim(libk::move(*reservation));
        KASSERT(reclaimed);
    }

    const auto kernel_vspace = kernel.initialize_kernel_vspace();
    KASSERT(kernel_vspace);
    kernel::diag::console::print<"kernel vspace: active\n">();
    const auto reclaimed_transition = kernel.pmm().reclaim(
        libk::move(*transition_reservation));
    KASSERT(reclaimed_transition);
    boot_info.transition = {};

    KASSERT(kernel.initialize_object_store());
    KASSERT(kernel.initialize_grants());

    kernel::boot::fdt::FDT_View fdt_view{};
    const auto fdt_pointer = kernel.direct_map().ptr<const byte>(
        boot_info.fdt.physical, boot_info.fdt.size);
    KASSERT(fdt_pointer);
    KASSERT(kernel::boot::fdt::init_view(fdt_view, fdt_pointer.value()));
    KASSERT(fdt_view.size == boot_info.fdt.size);

    const auto timebase = kernel::boot::parse_timebase_frequency(fdt_view);
    KASSERT(timebase);
    KASSERT(kernel.initialize_clock(timebase.value()));

    const auto summary = kernel::boot::parse_fdt_cpus_summary(
        fdt_view,
        boot_cpu);
    KASSERT(summary);

    auto builder_result = kernel.begin_cpus(summary.value());
    KASSERT(builder_result);
    auto builder = libk::move(builder_result).value();
    KASSERT(kernel::boot::populate_fdt_cpus(fdt_view, builder));
    KASSERT(builder.finish());
    KASSERT(kernel.initialize_kernel_domain(summary.value().count));

    auto& cpus = kernel.cpus();
    const kernel::CpuId boot_id = cpus.boot_id();
    kernel::mm::KernelVSpace& root = kernel.kernel_vspace();

    {
        kernel::CpuProvisioner provisioner{
            cpus,
            kernel.pmm(),
            kernel.objects(),
            kernel.clock()};

        // The boot CPU receives resources first so secondary allocation
        // pressure cannot remove the only execution context able to finish
        // bring-up.
        KASSERT(provisioner.prepare(
            boot_id,
            root,
            kernel::cpu_idle_entry));

        for (usize index = 0; index < cpus.count(); ++index) {
            const kernel::CpuId id{index};
            if (id == boot_id) {
                continue;
            }
            const kernel::CpuDescriptor* const cpu = cpus.descriptor(id);
            KASSERT(cpu != nullptr);
            if (cpu->availability() != kernel::CpuAvailability::Enabled) {
                continue;
            }
            // A failed secondary is canonicalized by prepare(); other CPUs
            // remain independently eligible for bring-up.
            (void)provisioner.prepare(
                id,
                root,
                kernel::cpu_idle_entry);
        }
    }

    const auto reclaimed_fdt = kernel.pmm().reclaim(
        libk::move(*fdt_reservation));
    KASSERT(reclaimed_fdt);
    boot_info.fdt = {};

    kernel::CpuRuntime* const boot_runtime = cpus.runtime(boot_id);
    KASSERT(boot_runtime != nullptr);
    KASSERT(cpus.begin_start(boot_id));
    KASSERT(kernel.start_reclaimer(*boot_runtime));
#if MYOS_INITIAL_USER_PROOF
    KASSERT(kernel.start_first_user(*boot_runtime));
#endif

    const usize cpu_stack = boot_runtime->stacks.init->top();
    KASSERT(cpu_stack != 0);
    KASSERT((cpu_stack & 0xfU) == 0);
    KASSERT(arch_boot_stack_guard_intact());

    arch::switch_to_stack_and_call(
        cpu_stack,
        kernel,
        *boot_runtime,
        kernel::boot_cpu_continue);
}

} // namespace kernel::boot
