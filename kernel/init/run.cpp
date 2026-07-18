#include <mm/virtual_layout.hpp>
#include <arch/boot_stack.hpp>
#include <arch/cpu.hpp>
#include <init/root_task.hpp>
#include <init/run.hpp>
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
#include <mm/kernel_stack.hpp>
#if MYOS_BUILTIN_TESTS
#include <test/test.hpp>
#endif

namespace {
constinit libk::ManualLifetime<kernel::KernelState> kernel_storage{};
constinit libk::ManualLifetime<kernel::init::RootTask> root_task_storage{};
// Init is the sole owner after accepting the architecture handoff.  No
// continuation borrows architecture boot storage.
constinit libk::ManualLifetime<kernel::boot::BootInfo> handoff_storage{};
// PMM mutates its bounded input while carving descriptor storage.  This is a
// one-shot work buffer, not another boot inventory: BootInfo remains the
// immutable handoff evidence until guarded-stack tests have consumed it.
constinit libk::ManualLifetime<kernel::mm::RegionList> pmm_map_storage{};

class ContinuationState final : private libk::noncopyable_nonmovable {
public:
    ContinuationState(
        kernel::KernelState& kernel,
        kernel::boot::BootInfo& boot,
        kernel::mm::BootReservation&& fdt,
        libk::optional<kernel::mm::BootReservation>&& module,
        kernel::KernelStack&& stack) noexcept
        : kernel(&kernel),
          boot(&boot),
          fdt(libk::move(fdt)),
          module(libk::move(module)),
          stack(libk::move(stack)) {}

    kernel::KernelState* kernel{};
    kernel::boot::BootInfo* boot{};
    kernel::mm::BootReservation fdt;
    libk::optional<kernel::mm::BootReservation> module{};
    kernel::KernelStack stack;
};

constinit libk::ManualLifetime<ContinuationState> continuation_storage{};
#if MYOS_BUILTIN_TESTS
void run_tests(const kernel::boot::BootInfo& boot_info) noexcept {
    const TestStats tests = run_builtin_tests(boot_info);
    KASSERT(arch_boot_stack_guard_intact());
    KASSERT(tests.failed == 0);
}
#endif

[[noreturn]] void continue_init(void* argument) noexcept {
    auto& state = *static_cast<ContinuationState*>(argument);
    kernel::KernelState& kernel = *state.kernel;
    kernel::boot::BootInfo& boot_info = *state.boot;
#if MYOS_BUILTIN_TESTS
    run_tests(boot_info);
#endif

    KASSERT(kernel.initialize_object_store());
    KASSERT(kernel.initialize_grants());

    KASSERT(boot_info.timebase_frequency != 0);
    KASSERT(kernel.initialize_clock(boot_info.timebase_frequency));
    KASSERT(boot_info.cpu);
    const kernel::CpuTopologySummary summary = boot_info.cpu.summary();

    auto builder_result = kernel.begin_cpus(summary);
    KASSERT(builder_result);
    auto builder = libk::move(builder_result).value();
    for (const kernel::boot::BootCpu& cpu : boot_info.cpu.cpus) {
        KASSERT(builder.append(cpu.hardware_id, cpu.availability));
    }
    KASSERT(builder.finish());
    KASSERT(kernel.initialize_kernel_domain(summary.count));

    // This is capacity deliberately withheld from userspace commitments. It
    // pays for CPU runtime, reclaimer and future kernel progress after the
    // root pool has promised the remaining capacity to init.
    constexpr usize system_base_pages = 256;
    constexpr usize system_pages_per_cpu = 64;
    const usize system_pages = system_base_pages
        + system_pages_per_cpu * summary.count;
    const usize free_pages = kernel.pmm().free_page_count();
    KASSERT(free_pages > system_pages);
    KASSERT(kernel.initialize_root_pool(kernel::resource::Budget{
        .memory = static_cast<u64>(free_pages - system_pages)
            * kernel::mm::page_size,
        .caps = 4096,
    }));

    if (boot_info.module) {
        KASSERT(state.module);
        auto pool = kernel.clone_root_pool();
        KASSERT(pool);
        KASSERT(kernel::init::RootTask::initialize_in(
            root_task_storage,
            kernel.objects(),
            kernel.pmm(),
            kernel.direct_map(),
            libk::move(pool).value(),
            *boot_info.module,
            libk::move(*state.module)));
        const auto bundle = root_task_storage->bundle();
        KASSERT(bundle);
        kernel::diag::console::print<
            "boot bundle: root={} segments={} bytes={}\n">(
            bundle.value().root_name(),
            bundle.value().segment_count(),
            bundle.value().bytes().size());
    }

    auto& cpus = kernel.cpus();
    const kernel::CpuId boot_id = cpus.boot_id();
    kernel::mm::KernelVSpace& root = kernel.kernel_vspace();

    {
        kernel::CpuProvisioner provisioner{
            cpus,
            kernel.pmm(),
            kernel.objects(),
            kernel.clock(),
            &kernel};

        // The boot CPU receives resources first so secondary allocation
        // pressure cannot remove the only execution context able to finish
        // bring-up.
        KASSERT(provisioner.prepare_boot(
            boot_id,
            root,
            state.stack,
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
        libk::move(state.fdt));
    KASSERT(reclaimed_fdt);

    // Every resource and normalized value from the handoff now has a runtime
    // owner.  End the one-shot handoff lifetime before execution is published.
    state.boot = nullptr;
    handoff_storage.reset();

    kernel::CpuRuntime* const boot_runtime = cpus.runtime(boot_id);
    KASSERT(boot_runtime != nullptr);
    KASSERT(cpus.begin_start(boot_id));

    KASSERT(kernel.start_reclaimer(*boot_runtime));
    if (root_task_storage) {
        const auto started = root_task_storage->start(kernel, *boot_runtime);
        if (!started) {
            kernel::diag::console::print<"root task failed: {}\n">(
                static_cast<u8>(started.error()));
        }
        KASSERT(started);
        kernel::diag::console::print<"root init: started\n">();
    }
    kernel::boot_cpu_continue(kernel, *boot_runtime);
}

} // namespace

namespace kernel::init {

[[noreturn]] void run(
    libk::ManualLifetime<kernel::boot::BootInfo>& source) noexcept {
    KASSERT(source);
    kernel::boot::BootInfo& boot_info =
        handoff_storage.emplace(libk::move(*source));
    source.reset();
    KASSERT(arch_boot_stack_guard_intact());

    auto& pmm_map = pmm_map_storage.emplace();
    for (const auto& region : boot_info.memory_regions) {
        KASSERT(pmm_map.try_push_back(region));
    }

    const auto initialized = kernel::KernelState::initialize_in(
        kernel_storage,
        libk::move(pmm_map),
        kernel::mm::DirectMapLayout{
            .physical_base = kernel::mm::PhysAddr{0},
            .virtual_base = kernel::mm::VirtAddr{kernel::mm::layout::DirectMapBegin},
            .window_size = kernel::mm::layout::DirectMapSize,
        });
    pmm_map_storage.reset();
    KASSERT(initialized);
    kernel::KernelState& kernel = *kernel_storage;

    KASSERT(boot_info.fdt);
    KASSERT(boot_info.transition);
    auto fdt = kernel.pmm().take_boot_reservation_for(boot_info.fdt.pages);
    KASSERT(fdt);
    auto transition =
        kernel.pmm().take_boot_reservation_for(boot_info.transition.pages);
    KASSERT(transition);
    libk::optional<kernel::mm::BootReservation> module{};
    if (boot_info.module) {
        module = kernel.pmm().take_boot_reservation_for(
            boot_info.module->pages);
        KASSERT(module);
    }
    while (auto reservation = kernel.pmm().take_boot_reservation()) {
        KASSERT(kernel.pmm().reclaim(libk::move(*reservation)));
    }

    KASSERT(kernel.initialize_kernel_vspace());
    kernel::diag::console::print<"kernel vspace: active\n">();
    KASSERT(kernel.pmm().reclaim(libk::move(*transition)));

    auto stack = kernel::KernelStack::create(kernel.kernel_vspace());
    KASSERT(stack);
    ContinuationState& continuation = continuation_storage.emplace(
        kernel,
        boot_info,
        libk::move(*fdt),
        libk::move(module),
        libk::move(stack).value());
    KASSERT(arch_boot_stack_guard_intact());
    arch::switch_to_stack_and_call(
        continuation.stack.top(),
        &continuation,
        continue_init);
}

} // namespace kernel::init
