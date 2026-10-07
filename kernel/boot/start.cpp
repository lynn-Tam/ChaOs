#include <optional>
#include <platform/riscv-virt/board.hpp>
#include <mm/table.hpp>
#include <cpu.hpp>
#include <console.hpp>
#include <boot/info.hpp>
#include <boot/link.hpp>
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
// Boot owns the firmware description until init consumes it.
constinit libk::ManualLifetime<BootInfo> handoff_storage{};
constinit libk::ManualLifetime<mm::RegionList> memory_storage{};

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
    libk_assert(virt_io_start(kernel.pool<io::Device>(), kernel.pool<mm::Mem>(), kernel.pool<irq::Irq>(), kernel.pmm(), boot_info, kernel.clock()));
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
    libk_assert(kernel.initialize_root_pool(resource::budget{
        .memory = static_cast<u64>(free_pages - system_pages)
            * mm::page_size,
        .caps = 4096,
    }));

    const auto module = boot_info.module;

    auto& cpus = kernel.cpus();
    const CpuId boot_id = cpus.boot_id();
    mm::KSpace& root = kernel.kernel_vspace();

    {
        CpuSetup provisioner{
            cpus,
            kernel.pmm(),
            kernel.pool<Thread>(),
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
    if (module) {
        libk_assert(state.module);
        const auto started = boot_root(kernel, *boot_runtime, *module, std::move(*state.module));
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

extern "C" [[noreturn]] void boot_enter(usize hart, usize pa) noexcept {
    libk_assert(pa < mm::DirectSize);
    auto& boot_info = handoff_storage.emplace();
    auto& memory = memory_storage.emplace();
    libk_assert(build_boot_info_from_fdt(boot_info, memory, CpuHwId{hart},
        mm::Phys{pa}, reinterpret_cast<const void*>(mm::DirectBegin + pa)));
    libk_assert(boot_guard_ok());

    // Freeze the board's external resource windows in the same inventory as
    // RAM before any physical Mem can borrow them. PMM allocates RAM only.
    const auto mmio = virt_mmio(boot_info);
    for (const auto& region : mmio)
        if (!region.range.empty()) libk_assert(memory.try_emplace_back(region));

    const auto initialized = KernelState::initialize_in(
        kernel_storage,
        std::move(memory),
        mm::Pmm::Window{
            .pa = mm::Phys{0},
            .va = mm::Virt{mm::DirectBegin},
            .size = mm::DirectSize,
        });
    memory_storage.reset();
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

    auto root = kernel_root(kernel.pmm());
    libk_assert(root);
    kernel.install_root(std::move(*root), kernel_begin().raw());
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
    libk_assert(boot_guard_ok());
    arch::switch_stack(
        continuation.stack.top(),
        &continuation,
        continue_init);
}
