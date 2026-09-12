#include <test/scenario.hpp>

#include <core/kernel_state.hpp>
#include <cpu/cpu_runtime.hpp>
#include <diag/console.hpp>
#include <mm/virtual_layout.hpp>
#include <sched/dispatcher.hpp>

namespace kernel::test::scenario::detail {
namespace {
template<typename T>
auto read(usize address) noexcept -> T {
    asm volatile("fence iorw, iorw" ::: "memory");
    const T value = *reinterpret_cast<volatile const T*>(address);
    asm volatile("fence iorw, iorw" ::: "memory");
    return value;
}
template<typename T>
void write(usize address, T value) noexcept {
    asm volatile("fence iorw, iorw" ::: "memory");
    *reinterpret_cast<volatile T*>(address) = value;
    asm volatile("fence iorw, iorw" ::: "memory");
}

// Test-only driver: one split-ring read with all addresses deliberately far
// from physical RAM. Success requires actual first-stage IOMMU translation.
void read_sector(io::DeviceLease& lease, mm::OwnedPage& backing,
    const time::Clock& clock, usize forbidden = 0,
    irq::Irq* interrupt = nullptr, ipc::Notification* notification = nullptr) noexcept {
    usize common{};
    usize notify{};
    usize notify_size{};
    usize isr{};
    u32 multiplier{};
    u8 cap = static_cast<u8>(lease.config32(0x34));
    for (usize count = 0; cap != 0 && count < 48; ++count) {
        const u32 header = lease.config32(cap);
        if ((header & 255) == 9) {
            const u8 type = header >> 24;
            if (type == 1 || type == 2 || type == 3) {
                const usize bar = lease.config32(cap + 4) & 255;
                KASSERT(bar < lease.bars().size());
                const usize offset = lease.config32(cap + 8);
                const usize size = lease.config32(cap + 12);
                const auto& region = lease.bars()[bar];
                KASSERT(offset <= region.size && size <= region.size - offset);
                const usize address = mm::layout::DirectMapBegin + region.address + offset;
                if (type == 1) {
                    KASSERT(size >= 56);
                    common = address;
                } else if (type == 2) {
                    KASSERT(((header >> 16) & 255) >= 20);
                    notify = address;
                    notify_size = size;
                    multiplier = lease.config32(cap + 16);
                } else {
                    KASSERT(size >= 1);
                    isr = address;
                }
            }
        }
        cap = static_cast<u8>(header >> 8);
    }
    KASSERT(common != 0 && notify != 0 && cap == 0);
    KASSERT(read<u8>(common + 20) == 0);
    write<u8>(common + 20, 1);
    write<u8>(common + 20, 3);
    write<u32>(common, 1);
    KASSERT((read<u32>(common + 4) & 3) == 3); // VERSION_1, ACCESS_PLATFORM
    write<u32>(common + 8, 0);
    write<u32>(common + 12, 0);
    write<u32>(common + 8, 1);
    write<u32>(common + 12, 3);
    write<u8>(common + 20, 11);
    KASSERT((read<u8>(common + 20) & 8) != 0);
    write<u16>(common + 22, 0);
    KASSERT(read<u16>(common + 24) >= 8);
    write<u16>(common + 24, 8);
    write<u16>(common + 26, 0xffff);
    write<u64>(common + 32, 0x1000);
    write<u64>(common + 40, 0x1100);
    write<u64>(common + 48, 0x1200);
    const usize queue_offset = usize{read<u16>(common + 30)} * multiplier;
    KASSERT(notify_size >= 2 && queue_offset <= notify_size - 2);

    auto* bytes = backing.bytes();
    for (usize index = 0; index < mm::page_size; ++index) bytes[index] = byte{};
    const usize ram = reinterpret_cast<usize>(bytes);
    write<u64>(ram, 0x1300);
    write<u32>(ram + 8, 16);
    write<u16>(ram + 12, 1);
    write<u16>(ram + 14, 1);
    write<u64>(ram + 16, forbidden != 0 ? forbidden : 0x1400);
    write<u32>(ram + 24, 512);
    write<u16>(ram + 28, 3);
    write<u16>(ram + 30, 2);
    write<u64>(ram + 32, 0x1600);
    write<u32>(ram + 40, 1);
    write<u16>(ram + 44, 2);
    for (usize index = 0; index < 512; ++index) bytes[0x400 + index] = byte{0xa5};
    write<u8>(ram + 0x600, 0xff);
    write<u16>(ram + 0x100, interrupt == nullptr ? 1 : 0);
    write<u16>(common + 28, 1);
    write<u8>(common + 20, 15);
    write<u16>(ram + 0x102, 1);
    write<u16>(notify + queue_offset, 0);
    const auto duration = clock.duration_from_nanoseconds(1'000'000'000);
    KASSERT(duration);
    const auto end = clock.now().checked_add(*duration);
    KASSERT(end);
    if (forbidden != 0) {
        for (;;) {
            if (const auto fault = lease.take_fault()) {
                KASSERT(fault->cause == 15 && fault->requester == 8);
                KASSERT(fault->address == forbidden);
                return; // close must drain the request after its DMA fault
            }
            KASSERT(clock.now() < *end);
        }
    }
    while (read<u16>(ram + 0x202) == 0) {
        if (const auto fault = lease.take_fault()) {
            diag::console::print<"[io] fault cause={} requester={:#x} address={:#x}\n">(
                fault->cause, fault->requester, fault->address);
            KASSERT(false);
        }
        KASSERT(clock.now() < *end);
    }
    KASSERT(read<u16>(ram + 0x202) == 1);
    KASSERT(read<u32>(ram + 0x204) == 0);
    KASSERT(read<u8>(ram + 0x600) == 0);
    for (usize index = 0; index < 512; ++index)
        KASSERT(read<u8>(ram + 0x400 + index) == 0);
    if (interrupt != nullptr) {
        KASSERT(notification != nullptr && isr != 0);
        for (;;) {
            auto taken = notification->take();
            if (taken) {
                KASSERT(taken.value().badges == 1);
                break;
            }
            KASSERT(taken.error() == ipc::NotificationError::Empty && clock.now() < *end);
            sched::yield();
        }
        auto delivery = interrupt->delivery();
        KASSERT(delivery && (read<u8>(isr) & 1) != 0);
        KASSERT(interrupt->ack(delivery.value().generation, delivery.value().sequence));
    }
}
struct BarUse final {
    mm::MemoryWork work{};
    libk::Atomic<bool> invalidated{};
    static void invalidate(void* context, mm::MemoryWork&& work, mm::MemoryInvalidation) noexcept {
        auto& use = *static_cast<BarUse*>(context);
        use.work = libk::move(work);
        use.invalidated.store<libk::MemoryOrder::Release>(true);
    }
    static void released(void*) noexcept {}
    inline static const mm::MemoryAttachmentOps ops{invalidate, released};
    mm::MemoryAttachment attachment{this, ops};
};

void space_lifetime(KernelState& kernel) noexcept {
    auto& objects = kernel.objects();
    auto& pmm = kernel.pmm();
    auto& device = kernel.io_platform().device();
    auto& executor = kernel.io_work();
    constexpr usize PageCount = 170; // crosses a PageLease metadata page
    constexpr auto Access = mm::AccessMask::of(mm::Access::Read, mm::Access::Write);
    for (usize test = 0; test < 8; ++test) {
        cap::GrantGraph graph{pmm};
        cap::CSpace caps{pmm};
        const resource::Budget limit{.memory = test == 5
            ? object::IoSpacePool::slot_charge().memory : 64 * mm::page_size, .caps = 4};
        auto pool_pending = objects.create_resource(limit);
        KASSERT(pool_pending);
        auto pool = libk::move(pool_pending).value().publish();
        auto pool_ref = pool.ref();
        KASSERT(pool_ref);
        auto charge = pool->reserve(libk::move(pool_ref).value(), object::IoSpacePool::slot_charge());
        KASSERT(charge);
        auto pending = objects.create_io_space_sponsored(
            libk::move(charge).value(), pmm, executor, objects, graph);
        KASSERT(pending);
        auto space = libk::move(pending).value().publish();
        if (test == 6) {
            auto permit_ref = pool.ref();
            auto charge_ref = pool.ref();
            auto target = space.ref();
            KASSERT(permit_ref && charge_ref && target);
            auto permit = pool->begin(libk::move(permit_ref).value());
            auto root_charge = pool->reserve(libk::move(charge_ref).value(), graph.node_charge());
            KASSERT(permit && root_charge);
            auto allocation = graph.create_allocation(permit.value(), libk::move(root_charge).value(),
                libk::move(target).value(), {cap::Rights::of(cap::Right::Close)});
            KASSERT(allocation);
            allocation.value().commit();
        }
        auto memory_pending = objects.create_anonymous(PageCount * mm::page_size, {.eager = true});
        KASSERT(memory_pending);
        auto memory = libk::move(memory_pending).value().publish();
        auto device_ref = kernel.io_platform().reference();
        auto memory_ref = memory.ref();
        KASSERT(device_ref && memory_ref);
        const cap::GrantCeiling dc{cap::Rights::of(cap::Right::Connect)};
        const cap::GrantCeiling mc{cap::Rights::of(cap::Right::Map),
            cap::MemoryAuthority{{0, PageCount}, Access, mm::MemoryTypes::of(mm::MemoryType::Normal)}};
        auto dg = graph.create_root(libk::move(device_ref).value(), dc);
        auto mg = graph.create_root(libk::move(memory_ref).value(), mc);
        KASSERT(dg && mg);
        const auto dk = dg.value().key();
        const auto mk = mg.value().key();
        auto dcap = caps.insert(libk::move(dg).value(), {dc.rights, dc.data});
        auto mcap = caps.insert(libk::move(mg).value(), {mc.rights, mc.data});
        KASSERT(dcap && mcap);
        const auto before = pool->available();
        {
            auto d = caps.resolve<io::Device>(dcap.value(), cap::Rights::of(cap::Right::Connect));
            auto m = caps.resolve<mm::MemoryObject>(mcap.value(), cap::Rights::of(cap::Right::Map));
            auto self = space.ref();
            KASSERT(d && m && self);
            auto result = space->bind(libk::move(self).value(), d.value(), m.value(), {0, PageCount}, 0x1000);
            if (test == 5) KASSERT(!result && result.error() == io::SpaceError::QuotaExceeded);
            else KASSERT(result);
        }
        cap::GrantRevoke revoke{};
        object::MemoryHold old_bar{};
        ipc::Notification notification{};
        object::IrqHold old_interrupt{};
        libk::optional<cap::CapHandle> interrupt_cap{};
        libk::optional<cap::CapHandle> bar_cap{};
        BarUse use{};
        if (test == 0) space->close(); // race opening with the real background executor
        else if (test != 5) {
            while (space->state() == io::SpaceState::Opening) sched::yield();
            KASSERT(space->state() == io::SpaceState::Active);
            auto info = space->info();
            KASSERT(info && info.value().configuration[0] == 0x1042'1af4);
            for (usize index = 0; index < 6; ++index)
                KASSERT((info.value().configuration[4 + index] & ~u32{15}) == 0);
            KASSERT(!device.acquire() && !memory->seal());
            KASSERT(pool->available().memory < before.memory);
            for (usize index = 0; index < 6 && !bar_cap; ++index) {
                auto grant = space->bar(index);
                if (!grant) {
                    KASSERT(grant.error() == io::SpaceError::InvalidRange);
                    continue;
                }
                auto lease = grant.value().acquire();
                KASSERT(lease);
                const auto ceiling = lease.value().ceiling();
                auto installed = caps.insert(libk::move(grant).value(), {ceiling.rights, ceiling.data});
                KASSERT(installed);
                bar_cap = installed.value();
                auto resolved = caps.resolve<mm::MemoryObject>(*bar_cap, cap::Rights::of(cap::Right::Map));
                KASSERT(resolved);
                auto ref = resolved.value().reference();
                KASSERT(ref);
                auto hold = libk::move(ref).value().into_hold<mm::MemoryObject>();
                KASSERT(hold);
                old_bar = libk::move(hold).value();
                auto page = old_bar->materialize(0);
                KASSERT(page && page.value().page().type == mm::MemoryType::Device);
            }
            KASSERT(bar_cap && !space->bar(6));
            {
                auto grant = space->interrupt();
                KASSERT(grant);
                auto lease = grant.value().acquire();
                KASSERT(lease);
                const auto ceiling = lease.value().ceiling();
                auto installed = caps.insert(libk::move(grant).value(), {ceiling.rights, ceiling.data});
                KASSERT(installed);
                interrupt_cap = installed.value();
                auto resolved = caps.resolve<irq::Irq>(*interrupt_cap, cap::Rights::of(cap::Right::Route));
                KASSERT(resolved);
                auto ref = resolved.value().reference();
                KASSERT(ref);
                auto hold = libk::move(ref).value().into_hold<irq::Irq>();
                KASSERT(hold);
                old_interrupt = libk::move(hold).value();
                KASSERT(old_interrupt->source().id() == 33);
                KASSERT(old_interrupt->bind(notification, 1));
            }
            if (test == 7) KASSERT(old_bar->attach(use.attachment, Access));
            if (test == 1) KASSERT(graph.invalidate(mk, revoke));
            if (test == 2) KASSERT(memory.retire());
            if (test == 3) KASSERT(space.retire());
            if (test == 4) KASSERT(graph.invalidate(dk, revoke));
            if (test == 6) KASSERT(pool->close() != resource::PoolState::Open);
            if (test == 7) space->close();
        }
        const auto duration = kernel.clock().duration_from_nanoseconds(1'000'000'000);
        KASSERT(duration);
        const auto end = kernel.clock().now().checked_add(*duration);
        KASSERT(end);
        if (test == 7) {
            while (!use.invalidated.load<libk::MemoryOrder::Acquire>()) {
                (void)graph.service(8);
                sched::yield();
                KASSERT(kernel.clock().now() < *end);
            }
            // An outstanding BAR memory use holds the hardware generation
            // even after capability admission has been revoked.
            KASSERT(old_bar->state() == mm::MemoryState::Stopping);
            KASSERT(space->state() != io::SpaceState::Closed && !device.acquire());
            KASSERT(!use.attachment.detach());
            use.work.reset();
        }
        while (space->state() != io::SpaceState::Closed
            || (revoke.initialized() && !revoke.complete())) {
            (void)graph.service(8);
            sched::yield();
            KASSERT(space->state() != io::SpaceState::Failed && kernel.clock().now() < *end);
        }
        if (bar_cap) {
            KASSERT(old_bar->state() == mm::MemoryState::Retired);
            KASSERT(!caps.resolve<mm::MemoryObject>(*bar_cap, cap::Rights::of(cap::Right::Map)));
            old_bar.reset();
            KASSERT(old_interrupt->state() == irq::State::Closed);
            KASSERT(!caps.resolve<irq::Irq>(*interrupt_cap, cap::Rights::of(cap::Right::Route)));
            old_interrupt.reset();
        }
        // Closed guarantees that hardware and mappings are reusable. Object
        // slots refund at the real background ObjectStore reclaim boundary.
        if (test != 6) {
            while (pool->available() != before) {
                (void)graph.service(8);
                sched::yield();
                KASSERT(kernel.clock().now() < *end);
            }
        }
        KASSERT(memory->attachment_count() == 0);
        auto available = device.acquire();
        KASSERT(available && (available->config32(4) & 4) == 0);
        available.reset();
        if (test != 3 && test != 6) KASSERT(space.retire());
        space.reset();
        caps.retire();
        while (graph.work_pending()) (void)graph.service(8);
        if (test != 2) KASSERT(memory.retire());
        memory.reset();
        while (pool->available() != limit) {
            KASSERT(kernel.clock().now() < *end);
            (void)graph.service(8);
            sched::yield();
        }
        // The final refund may still be running ResourcePool::service on
        // another CPU. close joins that owner; full budget is not its exit.
        (void)pool->close();
        while (pool->state() != resource::PoolState::Closed) {
            KASSERT(kernel.clock().now() < *end);
            sched::yield();
        }
        KASSERT(pool.retire());
        pool.reset();
    }
    diag::console::print<"[scenario] io-space ok: source revoke, backing retire, pool close, cancel, quota, refund\n">();
}
} // namespace

auto io_lease(CpuRuntime& runtime) noexcept -> bool {
    KASSERT(runtime.kernel != nullptr);
    auto& kernel = *runtime.kernel;
    auto& platform = kernel.io_platform();
    KASSERT(platform.present());
    auto& device = platform.device();
    auto& pmm = kernel.pmm();
    auto backing = pmm.allocate_page();
    KASSERT(backing);
    auto guard = pmm.allocate_page();
    KASSERT(guard);
    for (usize index = 0; index < mm::page_size; ++index)
        guard.value().bytes()[index] = byte{0xd3};
    const mm::Page pages[] = {backing.value().page()};
    using State = io::DeviceLease::State;
    // No driver or CPU BAR mapping exists in this scenario. The test keeps
    // the DMA frame alive across every context publication and close fence.
    for (usize generation = 0; generation < 5; ++generation) {
        auto lease = device.acquire();
        KASSERT(lease && !device.acquire());
        ipc::Notification notification{};
        irq::Irq interrupt{irq::SourceToken::from_bootstrap(lease->irq_source())};
        const bool use_interrupt = generation == 0 || generation == 4;
        if (use_interrupt) KASSERT(interrupt.bind(notification, 1));
        auto root = arch::IoRoot::create(pmm, mm::page_size, pages, true);
        KASSERT(root);
        const usize before = pmm.stats().free_pages;
        const usize tables = root.value().page_count();
        lease->open(libk::move(root).value());
        if (generation == 1) lease->close();
        else {
            while (lease->poll() == State::Opening) {}
            KASSERT(lease->state() == State::Active);
            KASSERT((lease->config32(4) & 4) != 0);
            read_sector(*lease, backing.value(), kernel.clock(),
                generation == 3 ? guard.value().page().base().raw() : 0,
                use_interrupt ? &interrupt : nullptr, use_interrupt ? &notification : nullptr);
            KASSERT(interrupt.close());
            lease->close();
        }
        KASSERT(!device.acquire());
        while (lease->poll() != State::Closed) {
            KASSERT(lease->state() != State::Failed);
        }
        KASSERT(pmm.stats().free_pages == before + tables);
        for (usize index = 0; index < mm::page_size; ++index)
            KASSERT(guard.value().bytes()[index] == byte{0xd3});
        KASSERT(!device.acquire()); // closed token still owns the reservation
    }
    auto next = device.acquire();
    KASSERT(next && (next->config32(4) & 4) == 0);
    next.reset();
    space_lifetime(kernel);
    bool stopped{};
    auto retiring = device.acquire(&stopped, [](void* context) noexcept {
        *static_cast<bool*>(context) = true;
    });
    KASSERT(retiring);
    auto reference = platform.reference();
    KASSERT(reference && reference.value().retire());
    KASSERT(stopped && !device.acquire());
    KASSERT(!reference.value().pin<io::Device>());
    retiring->close();
    diag::console::print<"[scenario] io-lease ok: open, cancel, reset, fence, reuse\n">();
    diag::console::print<"[scenario] io-dma ok: translated block read before and after reuse\n">();
    diag::console::print<"[scenario] io-fault ok: physical DMA denied, guard intact, recovery read\n">();
    diag::console::print<"[scenario] io-irq ok: INTx notification, exact ack, reuse\n">();
    return true;
}

} // namespace kernel::test::scenario::detail
