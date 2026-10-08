#include <optional>
#include <platform/boot.hpp>
#include <mm/table.hpp>
#include <cpu.hpp>
#include <console.hpp>
#include <boot/info.hpp>
#include <boot/start.hpp>
#include <libk/assert.hpp>
#include <base/types.hpp>
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
        auto physical = boot_layout.phys(mm::Virt{begin});
        if (!physical) return std::unexpected(PtErr::BadPhys);
        auto pages = mm::Pages::from_aligned_bytes(*physical, end - begin);
        if (!pages) return std::unexpected(PtErr::BadPhys);
        auto installed = root->map(mm::Virt{begin}, *pages, s.perms);
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
        auto installed = root->map(mm::Virt{mm::DirectBegin + pages.base().base().raw()}, pages, PtPerm::Rw);
        if (!installed) return std::unexpected(installed.error());
    }
    // Secondaries enter physically; these leaves bridge to their high entry.
    const auto pages = boot_layout.secondary.pages();
    auto installed = root->map(mm::Virt{pages.base().base().raw()}, pages, PtPerm::Rx);
    if (!installed) return std::unexpected(installed.error());
    return root;
}


static constinit libk::ManualLifetime<mm::Pmm> pmm_storage{};
static constinit libk::ManualLifetime<mm::KSpace> vm_storage{};
static constinit libk::ManualLifetime<Boot> boot_storage{};
// Boot owns the firmware description until init consumes it.
static constinit libk::ManualLifetime<BootInfo> handoff_storage{};
static constinit libk::ManualLifetime<mm::RegionList> memory_storage{};

struct BootStack {
    mm::BootPages scratch, firmware;
    std::optional<mm::BootPages> module;
    mm::Stack stack;
};
static constinit libk::ManualLifetime<BootStack> stack_storage{};

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
    auto budget = boot.clock.duration_from_nanoseconds(1'000'000);
    auto period = boot.clock.duration_from_nanoseconds(10'000'000);
    auto urgency = sched::Urgency::make(31);
    libk_assert(budget && period && urgency);
    auto sc = boot.objects.get<sched::Sc>().create(
        sched::Sc::Config{.budget = *budget, .period = *period, .urgency = *urgency},
        boot.clock.now());
    libk_assert(sc);
    boot.worker.sc = std::move(*sc).publish();
    auto target = boot.worker.thread.clone();
    libk_assert(target && boot.domain->admit(boot.worker.sc.get(), cpu.id));
    libk_assert(boot.worker.sc->bind(std::move(*target)));
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

    libk_assert(boot_info.timebase_frequency != 0);
    libk_assert(boot.clock.valid());
#if TEST_ENABLED
    test::boot = &boot;
#endif
    libk_assert(!boot_info.cpus.empty());
    libk_assert(boot.cpus.add(boot_info.cpus[0], boot.objects, boot.grants,
                              boot.vm, boot.clock, &state.stack));
    for (auto hart : boot_info.cpus.span() | std::views::drop(1))
        if (!boot.cpus.add(hart, boot.objects, boot.grants, boot.vm, boot.clock))
            console::print<"cpu: prepare hart={} failed\n">(hart.raw);
    BootHw hw;
    auto& boot_cpu = *boot.cpus.get(Cpus::boot_id());
    libk_assert(platform_start(boot.pmm, boot.clock, hw));
    boot_cpu.ext_irq = hw.ext_irq;
    const auto rights = [](auto... extra) noexcept {
        return cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate,
            cap::Right::Inspect, cap::Right::Revoke, extra...);
    };
    for (const auto& r : hw.resources) {
        auto entry = r.entry;
        const bool added = std::visit([&](auto value) noexcept {
            using T = decltype(value);
            auto emit = [&](auto pending, cap::View view) noexcept {
                if (!pending) return false;
                auto ref = std::move(*pending).publish().erase();
                return ref && boot.resources.try_emplace_back(entry, std::move(*ref), view);
            };
            if constexpr (std::is_same_v<T, io::Reg>) {
                entry.phys = value.pa;
                entry.bytes = value.size;
                const auto pages = mm::Pages::from_aligned_bytes(mm::Phys{value.pa}, value.size);
                if (!pages) return false;
                const mm::Extent extent{{0, pages->page_count()}, *pages, value.perms};
                return emit(boot.objects.get<mm::Mem>().create(boot.pmm,
                    value.size, mm::PhysCfg{{&extent, 1}, {}}),
                    {rights(cap::Right::Map), cap::MemLimit{extent.object, extent.perms}});
            } else if constexpr (std::is_same_v<T, irq::Line>) {
                return emit(boot.objects.get<irq::Irq>().create(value),
                    {rights(cap::Right::Route, cap::Right::Observe, cap::Right::Ack,
                            cap::Right::Close), cap::IrqRoute{value.id, value.level}});
            } else {
                return emit(boot.objects.get<io::Host>().create(*value),
                    {rights(cap::Right::Connect), cap::HostLimit{0, value->count}});
            }
        }, r.value);
        libk_assert(added);
    }
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
    auto root = boot.objects.get<object::group>().create(boot.pmm, resource::budget{
        .memory = static_cast<u64>(free_pages - system_pages)
            * mm::page_size,
        .caps = 4096,
    });
    libk_assert(root);
    boot.root = std::move(*root).publish();

    const auto module = boot_info.module;

    auto& cpus = boot.cpus;
    const CpuId boot_id = cpus.boot_id();
    // Preserve the original firmware bytes for the trusted root. The physical
    // reservation becomes ordinary Mem ownership instead of being reclaimed.
    auto firmware = boot.pmm.adopt(std::move(state.firmware));
    libk_assert(firmware);
    const mm::Extent extent{{0, firmware->page_count()}, boot_info.firmware.pages,
                            mm::Perms::of(mm::Perm::Read)};
    auto source = boot.objects.get<mm::Mem>().create(boot.pmm,
        firmware->page_count() * mm::page_size, mm::PhysCfg{{&extent, 1}, std::move(*firmware)});
    libk_assert(source);
    auto ref = std::move(*source).publish().erase();
    libk_assert(ref && boot.resources.try_emplace_back(
        BootCap{0, 0x46445420, 1, 0, OBJECT_KIND_MEMORY, 0, 0, "firmware",
                boot_info.firmware.physical.raw(), boot_info.firmware.size},
        std::move(*ref), cap::View{rights(cap::Right::Map), cap::MemLimit{extent.object, extent.perms}}));

    // Every resource and normalized value from the handoff now has a runtime
    // owner.  End the one-shot handoff lifetime before execution is published.
    handoff_storage.reset();

    Cpu* const boot_runtime = cpus.get(boot_id);
    libk_assert(boot_runtime != nullptr);

    start_worker(boot, *boot_runtime);
    if (module) {
        libk_assert(state.module);
        const auto started = boot_root(boot, *boot_runtime, *module, std::move(*state.module));
        if (!started) {
            console::print<"root task failed: {}\n">(
                static_cast<u8>(started.error()));
        }
        libk_assert(started);
        console::print<"root init: started\n">();
    }
    boot_runtime->install(boot_runtime->hw.raw);
    console::print<"trap install ok\n">();
    boot.cpus.start();
    boot_runtime->dispatcher().enter_idle();
}

extern "C" [[noreturn]] void boot_enter(usize hart, usize pa) noexcept {
    libk_assert(pa < mm::DirectSize);
    auto& boot_info = handoff_storage.emplace();
    auto& memory = memory_storage.emplace();
    libk_assert(platform_boot(boot_info, memory, CpuHwId{hart}, mm::Phys{pa}));

    const auto initialized = mm::Pmm::initialize_in(
        pmm_storage,
        std::move(memory),
        mm::Pmm::Window{
            .pa = mm::Phys{0},
            .va = mm::Virt{mm::DirectBegin},
            .size = mm::DirectSize,
        });
    memory_storage.reset();
    libk_assert(initialized);
    auto& pmm = *pmm_storage;

    libk_assert(boot_info.firmware);
    auto firmware = pmm.take_boot(boot_info.firmware.pages);
    libk_assert(firmware);
    auto scratch = pmm.take_boot(boot_layout.scratch.pages());
    libk_assert(scratch);
    std::optional<mm::BootPages> module{};
    if (boot_info.module) {
        module = pmm.take_boot(
            boot_info.module->pages);
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
    auto& state = stack_storage.emplace(std::move(*scratch), std::move(*firmware), std::move(module), std::move(*stack));
    arch::switch_stack(state.stack.top(), &state, continue_init);
}
