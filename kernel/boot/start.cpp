#include <optional>
#include <mm/table.hpp>
#include <cpu.hpp>
#include <console.hpp>
#include <boot/info.hpp>
#include <boot/start.hpp>
#include <libk/assert.hpp>
#include <libk/types.hpp>
#include <cpu/cpu.hpp>
#include <libk/manual_lifetime.hpp>
#include <utility>
#include <mm/kspace.hpp>
#if TEST_ENABLED
#include <test/boot.hpp>
#endif

extern "C" {
extern char kernel_text_start[], kernel_text_end[];
extern char kernel_rodata_start[], kernel_rodata_end[];
extern char kernel_data_start[], kernel_data_end[];
extern char kernel_bss_start[], kernel_bss_end[];
}

auto kernel_root(mm::Pmm& pmm) noexcept -> std::expected<mm::PageTable, mm::PtErr> {
    using mm::PtPerm;
    using mm::PtErr;
    auto root = mm::PageTable::create(pmm, mm::PageTable::Kind::Kernel);
    if (!root) return std::unexpected(root.error());
    const struct Section {
        const char* begin;
        const char* end;
        PtPerm perms;
    } sections[] = {
        {kernel_text_start, kernel_text_end, PtPerm::Rx},
        {kernel_rodata_start, kernel_rodata_end, PtPerm::Ro},
        {kernel_data_start, kernel_data_end, PtPerm::Rw},
        {kernel_bss_start, kernel_bss_end, PtPerm::Rw},
    };
    for (const auto& s : sections) {
        const usize begin = reinterpret_cast<usize>(s.begin), end = reinterpret_cast<usize>(s.end);
        libk_assert(begin <= end && begin % mm::page_size == 0 && end % mm::page_size == 0);
        if (begin == end) continue;
        auto physical = boot_layout.phys(mm::va_t{begin});
        if (!physical) return std::unexpected(PtErr::BadPhys);
        auto pages = mm::Pages::from_aligned_bytes(*physical, end - begin);
        if (!pages) return std::unexpected(PtErr::BadPhys);
        auto installed = root->map(mm::va_t{begin}, *pages, s.perms);
        if (!installed) return std::unexpected(installed.error());
    }
    for (auto pages : pmm.ram()) {
        auto va = pmm.virt(pages.base().base(), pages.page_count() * mm::page_size);
        libk_assert(va);
        auto installed = root->map(*va, pages, PtPerm::Rw);
        if (!installed) return std::unexpected(installed.error());
    }
    // MMIO has the same immutable resource inventory, but is not allocatable RAM.
    for (const auto& region : pmm.regions()) {
        if (region.is_ram()) continue;
        const auto pages = region.range;
        auto installed = root->map(mm::va_t{mm::DirectBegin + pages.base().base().raw()}, pages, PtPerm::Rw);
        if (!installed) return std::unexpected(installed.error());
    }
    // Secondaries enter physically; these leaves bridge to their high entry.
    const auto pages = boot_layout.secondary.pages();
    auto installed = root->map(mm::va_t{pages.base().base().raw()}, pages, PtPerm::Rx);
    if (!installed) return std::unexpected(installed.error());
    return root;
}


static constinit libk::manual_lifetime<mm::Pmm> pmm_storage{};
static constinit libk::manual_lifetime<mm::KSpace> vm_storage{};
static constinit libk::manual_lifetime<Boot> boot_storage{};
// Boot owns the firmware description until init consumes it.
static constinit libk::manual_lifetime<BootInfo> handoff_storage{};
static constinit libk::manual_lifetime<mm::RegionList> memory_storage{};

struct BootStack {
    mm::BootPages scratch, firmware;
    std::optional<mm::BootPages> module;
    mm::Stack stack;
    HwStart start;
};
static constinit libk::manual_lifetime<BootStack> stack_storage{};

[[noreturn]] static void work_entry(void* arg) noexcept {
    auto& work = *static_cast<WorkQueue*>(arg);
    for (;;) {
        (void)work.run();
        if (work.arm()) sched::block();
        else sched::yield();
    }
}

static void start_worker(Boot& boot, Cpu& cpu) noexcept {
    auto stack = mm::Stack::create(boot.vm);
    libk_assert(stack);
    auto thread = boot.objects.get<Thread>().create(std::move(*stack), Env::kernel(boot.vm),
        Thread::KernelStart{work_entry, &boot.work});
    libk_assert(thread);
    boot.worker.thread = std::move(*thread).publish();
    auto budget = boot.clock.ticks<1, time::ms>();
    auto period = boot.clock.ticks<10, time::ms>();
    auto urgency = sched::Urgency::make(31);
    libk_assert(budget && period && urgency);
    auto sc = boot.objects.get<sched::Sc>().create(
        sched::Sc::Config{.budget = *budget, .period = *period, .urgency = *urgency},
        boot.clock.rd_ticks());
    libk_assert(sc);
    boot.worker.sc = std::move(*sc).publish();
    libk_assert(boot.domain->admit(boot.worker.sc.get(), cpu.id));
    libk_assert(boot.worker.sc->bind(boot.worker.thread));
    libk_assert(cpu.dispatcher().make_ready(boot.worker.sc.get()));
    boot.work.bind(Work::Fn::bind(boot.worker));
}

[[noreturn]] static void continue_init(void* argument) noexcept {
    auto& state = *static_cast<BootStack*>(argument);
    libk_assert(pmm_storage->reclaim(std::move(state.scratch)));
    auto& boot_info = *handoff_storage;
    auto& boot = boot_storage.emplace(*pmm_storage, *vm_storage, boot_info.timebase_frequency);
#if TEST_ENABLED
    test::run(boot_info, boot.pmm);
#endif

#if TEST_ENABLED
    test::boot = &boot;
#endif
    libk_assert(!boot_info.cpus.empty());
    libk_assert(boot.cpus.add(boot_info.cpus[0], boot.objects, boot.grants,
                              boot.vm, boot.clock, &state.stack));
    for (auto hart : boot_info.cpus.span() | std::views::drop(1))
        if (!boot.cpus.add(hart, boot.objects, boot.grants, boot.vm, boot.clock))
            console::print<"cpu: prepare hart={} failed\n">(hart.raw);
    auto domain = boot.objects.get<sched::Domain>().create(
        boot.cpus.count(), sched::Domain::share_scale, 100'000U);
    libk_assert(domain);
    boot.domain = std::move(*domain).publish();

    // This is capacity deliberately withheld from userspace commitments. It
    // pays for CPU runtime, cleanup executor and future kernel progress after the
    // root pool has promised the remaining capacity to init.
    constexpr usize system_base_pages = 256;
    constexpr usize system_pages_per_cpu = 64;
    const usize system_pages = system_base_pages
        + system_pages_per_cpu * boot.cpus.count();
    const usize free_pages = boot.pmm.free_page_count();
    libk_assert(free_pages > system_pages);
    auto root = boot.objects.get<object::group>().create(resource::budget{
        .memory = static_cast<u64>(free_pages - system_pages)
            * mm::page_size,
        .caps = 4096,
    });
    libk_assert(root);
    boot.root = std::move(*root).publish();

    auto& runtime = *boot.cpus.get(Cpus::boot_id());
    start_worker(boot, runtime);
    if (boot_info.module) {
        libk_assert(state.module);
        const auto started = boot_root(boot, runtime, boot_info,
            std::move(*state.module), std::move(state.firmware), state.start);
        if (!started)
            console::print<"root task failed: {}\n">(static_cast<u8>(started.error()));
        libk_assert(started);
        console::print<"root init: started\n">();
    } else {
        // Kernel-only runs need the controller, but publish no user resources.
        auto discard = [](u32, u32, HwRes) noexcept { return true; };
        runtime.ext_irq = state.start(boot.pmm, boot.clock, HwEmit::bind(discard));
        libk_assert(runtime.ext_irq && boot.pmm.reclaim(std::move(state.firmware)));
    }
    handoff_storage.reset();
    runtime.install(runtime.hw.raw);
    console::print<"trap install ok\n">();
    boot.cpus.start();
    runtime.dispatcher().enter_idle();
}

[[noreturn]] void boot_start(CpuHwId hart, mm::pa_t pa, BootRead read, HwStart start) noexcept {
    libk_assert(pa.raw() < mm::DirectSize);
    auto& boot_info = handoff_storage.emplace();
    auto& memory = memory_storage.emplace();
    libk_assert(read(boot_info, memory, hart, pa));
    using Kind = mm::Region::Kind;
    for (auto r : {mm::Region{boot_layout.entry.pages(), Kind::Boot},
                   mm::Region{boot_layout.scratch.pages(), Kind::Boot},
                   mm::Region{boot_layout.secondary.pages(), Kind::Kernel},
                   mm::Region{boot_layout.kernel.pages(), Kind::Kernel}})
        libk_assert(memory.try_push_back(r));
    libk_assert(mm::normalize(memory));

    const auto initialized = mm::Pmm::initialize_in(
        pmm_storage,
        std::move(memory),
        mm::Pmm::Window{
            .pa = mm::pa_t{0},
            .va = mm::va_t{mm::DirectBegin},
            .size = mm::DirectSize,
        });
    memory_storage.reset();
    libk_assert(initialized);
    auto& pmm = *pmm_storage;

    libk_assert(boot_info.firmware);
    auto firmware = pmm.take_boot(boot_info.firmware.pages());
    libk_assert(firmware);
    auto scratch = pmm.take_boot(boot_layout.scratch.pages());
    libk_assert(scratch);
    std::optional<mm::BootPages> module{};
    if (boot_info.module) {
        module = pmm.take_boot(
            boot_info.module->pages());
        libk_assert(module);
    }
    while (auto reservation = pmm.take_boot()) {
        libk_assert(pmm.reclaim(std::move(*reservation)));
    }

    auto root = kernel_root(pmm);
    libk_assert(root);
    auto& vm = vm_storage.emplace(pmm, std::move(*root), boot_layout.va);
    arch::activate_root(vm.cpu_root());
    console::print<"kernel vspace: active\n">();

    auto stack = mm::Stack::create(vm);
    libk_assert(stack);
    auto& state = stack_storage.emplace(std::move(*scratch), std::move(*firmware), std::move(module), std::move(*stack), start);
    arch::switch_stack(state.stack.top(), &state, continue_init);
}
