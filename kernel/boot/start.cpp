#include <optional>
#include <platform/riscv-virt/board.hpp>
#include <mm/table.hpp>
#include <arch/boot_stack.hpp>
#include <arch/cpu.hpp>
#include <boot/root.hpp>
#include <boot/start.hpp>
#include <console.hpp>
#include <boot/info.hpp>
#include <state.hpp>
#include <libk/assert.hpp>
#include <base/types.hpp>
#include <cpu/setup.hpp>
#include <cpu/registry.hpp>
#include <cpu/runtime.hpp>
#include <cpu/start.hpp>
#include <libk/manual_lifetime.hpp>
#include <utility>
#include <mm/kspace.hpp>
#if TEST_ENABLED
#include <test/boot.hpp>
#endif

namespace {
constinit libk::ManualLifetime<KernelState> kernel_storage{};
constinit libk::ManualLifetime<RootTask> root_task_storage{};
// Init is the sole owner after accepting the architecture handoff.  No
// continuation borrows architecture boot storage.
constinit libk::ManualLifetime<BootInfo> handoff_storage{};

class ContinuationState final : private libk::noncopyable_nonmovable {
public:
    ContinuationState(
        KernelState& kernel,
        BootInfo& boot,
        mm::BootPages&& fdt,
        std::optional<mm::BootPages>&& module,
        mm::Stack&& stack) noexcept
        : kernel(&kernel),
          boot(&boot),
          fdt(std::move(fdt)),
          module(std::move(module)),
          stack(std::move(stack)) {}

    KernelState* kernel{};
    BootInfo* boot{};
    mm::BootPages fdt;
    std::optional<mm::BootPages> module{};
    mm::Stack stack;
};

constinit libk::ManualLifetime<ContinuationState> continuation_storage{};
[[noreturn]] void continue_init(void* argument) noexcept {
    auto& state = *static_cast<ContinuationState*>(argument);
    KernelState& kernel = *state.kernel;
    BootInfo& boot_info = *state.boot;
#if TEST_ENABLED
    test::run(boot_info, kernel.pmm());
#endif

    libk_assert(kernel.initialize_objects());
    libk_assert(kernel.initialize_grants());

    libk_assert(boot_info.timebase_frequency != 0);
    libk_assert(kernel.initialize_clock(boot_info.timebase_frequency));
    libk_assert(virt_io_start(kernel.io(), kernel.pmm(), boot_info, kernel.clock()));
    libk_assert(boot_info.cpu);
    const CpuTopo summary = boot_info.cpu.summary();

    auto builder_result = kernel.begin_cpus(summary);
    libk_assert(builder_result);
    auto builder = std::move(builder_result).value();
    for (const BootCpu& cpu : boot_info.cpu.cpus) {
        libk_assert(builder.append(cpu.hardware_id, cpu.availability));
    }
    libk_assert(builder.finish());
    libk_assert(kernel.initialize_kernel_domain(summary.count));

    // This is capacity deliberately withheld from userspace commitments. It
    // pays for CPU runtime, cleanup executor and future kernel progress after the
    // root pool has promised the remaining capacity to init.
    constexpr usize system_base_pages = 256;
    constexpr usize system_pages_per_cpu = 64;
    const usize system_pages = system_base_pages
        + system_pages_per_cpu * summary.count;
    const usize free_pages = kernel.pmm().free_page_count();
    libk_assert(free_pages > system_pages);
    /*luna change: remove the one-shot pressure capacity probe, reason: the 48M root-budget sample is recorded and the temporary diagnostic must not remain in admission*/
    libk_assert(kernel.initialize_root_pool(resource::budget{
        .memory = static_cast<u64>(free_pages - system_pages)
            * mm::page_size,
        .caps = 4096,
    }));

    if (boot_info.module) {
        libk_assert(state.module);
        auto pool = kernel.clone_root_pool();
        libk_assert(pool);
        libk_assert(RootTask::initialize_in(
            root_task_storage,
            kernel.pool<mm::Mem>(),
            kernel.pmm(),
            kernel.direct_map(),
            std::move(pool).value(),
            *boot_info.module,
            std::move(*state.module)));
        const auto bundle = root_task_storage->bundle();
        libk_assert(bundle);
        console::print<
            "boot bundle: root={} segments={} bytes={}\n">(
            bundle.value().root_name(),
            bundle.value().segment_count(),
            bundle.value().bytes().size());
    }

    auto& cpus = kernel.cpus();
    const CpuId boot_id = cpus.boot_id();
    mm::KSpace& root = kernel.kernel_vspace();

    {
        CpuSetup provisioner{
            cpus,
            kernel.pmm(),
            kernel.tasks().threads,
            kernel.clock(),
            &kernel};

        // The boot CPU receives resources first so secondary allocation
        // pressure cannot remove the only execution context able to finish
        // bring-up.
        libk_assert(provisioner.prepare_boot(
            boot_id,
            root,
            state.stack,
            cpu_idle_entry));

        for (usize index = 0; index < cpus.count(); ++index) {
            const CpuId id{index};
            if (id == boot_id) {
                continue;
            }
            const CpuDescriptor* const cpu = cpus.descriptor(id);
            libk_assert(cpu != nullptr);
            if (cpu->availability() != CpuAvail::Enabled) {
                continue;
            }
            // A failed secondary is canonicalized by prepare(); other CPUs
            // remain independently eligible for bring-up.
            (void)provisioner.prepare(
                id,
                root,
                cpu_idle_entry);
        }
    }

    const auto reclaimed_fdt = kernel.pmm().reclaim(
        std::move(state.fdt));
    libk_assert(reclaimed_fdt);

    // Every resource and normalized value from the handoff now has a runtime
    // owner.  End the one-shot handoff lifetime before execution is published.
    state.boot = nullptr;
    handoff_storage.reset();

    CpuRuntime* const boot_runtime = cpus.runtime(boot_id);
    libk_assert(boot_runtime != nullptr);
    libk_assert(cpus.begin_start(boot_id));

    libk_assert(kernel.start_cleanup(*boot_runtime));
    if (root_task_storage) {
        const auto started = root_task_storage->start(kernel, *boot_runtime);
        if (!started) {
            console::print<"root task failed: {}\n">(
                static_cast<u8>(started.error()));
        }
        libk_assert(started);
        console::print<"root init: started\n">();
    }
    boot_cpu_continue(kernel, *boot_runtime);
}

} // namespace

[[noreturn]] void start_kernel(
    libk::ManualLifetime<BootInfo>& source,
    libk::ManualLifetime<mm::RegionList>& memory) noexcept {
    libk_assert(source);
    BootInfo& boot_info =
        handoff_storage.emplace(std::move(*source));
    source.reset();
    libk_assert(arch_boot_stack_guard_intact());

    // Freeze the board's external resource windows in the same inventory as
    // RAM before any physical Mem can borrow them. PMM allocates RAM only.
    const auto mmio = virt_mmio(boot_info);
    for (const auto& region : mmio)
        if (!region.range.empty()) libk_assert(memory->try_emplace_back(region));

    const auto initialized = KernelState::initialize_in(
        kernel_storage,
        std::move(*memory),
        mm::DirectMap::Layout{
            .physical_base = mm::Phys{0},
            .virtual_base = mm::Virt{mm::DirectBegin},
            .window_size = mm::DirectSize,
        });
    memory.reset();
    libk_assert(initialized);
    KernelState& kernel = *kernel_storage;

    libk_assert(boot_info.fdt);
    libk_assert(boot_info.transition.valid());
    auto fdt = kernel.pmm().take_boot(boot_info.fdt.pages);
    libk_assert(fdt);
    auto transition =
        kernel.pmm().take_boot(boot_info.transition);
    libk_assert(transition);
    std::optional<mm::BootPages> module{};
    if (boot_info.module) {
        module = kernel.pmm().take_boot(
            boot_info.module->pages);
        libk_assert(module);
    }
    while (auto reservation = kernel.pmm().take_boot()) {
        libk_assert(kernel.pmm().reclaim(std::move(*reservation)));
    }

    libk_assert(kernel.initialize_kernel_vspace());
    console::print<"kernel vspace: active\n">();
    libk_assert(kernel.pmm().reclaim(std::move(*transition)));

    auto stack = mm::Stack::create(kernel.kernel_vspace());
    libk_assert(stack);
    ContinuationState& continuation = continuation_storage.emplace(
        kernel,
        boot_info,
        std::move(*fdt),
        std::move(module),
        std::move(stack).value());
    libk_assert(arch_boot_stack_guard_intact());
    arch::switch_to_stack_and_call(
        continuation.stack.top(),
        &continuation,
        continue_init);
}
