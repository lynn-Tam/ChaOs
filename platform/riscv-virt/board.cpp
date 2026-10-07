#include <expected>
#include <utility>
#include "board.hpp"
#include "config.hpp"
#include "dma.hpp"
#include <platform/plic.hpp>
#include <csr.hpp>
#include <console.hpp>
#include <trap.hpp>
#include <libk/manual_lifetime.hpp>
#include <mm/table.hpp>

static constinit libk::ManualLifetime<Plic> plic{};

// The IOMMU owns its controller storage and the identities behind it. Nodes
// are funded from PMM, independent of interrupt numbers or a fixed registry.
struct PciBus final {
    PciBus(mm::Pmm& pmm, usize base) noexcept
        : iommu(base), nodes(pmm.group()),
          faults({plic->routes(), VirtIommuIrq},
                 irq::Route::Handler::bind<&Iommu::interrupt>(iommu)) {}
    Iommu iommu;
    mm::PageGroup nodes;
    irq::Route faults;
};
static libk::ManualLifetime<PciBus> pci{};

struct Uart final {
    object::ref<mm::Mem> memory;
    object::ref<irq::Irq> irq;
};
static libk::ManualLifetime<Uart> uart{};

void virt_irq_start(usize hart) noexcept {
    plic->start(mm::DirectBegin + VirtPlicBase, hart * 2 + 1);
    csr::Sie::enable_external();
}
void virt_irq() noexcept {
    plic->dispatch();
}

auto virt_io_start(object::pool<io::Device>& devices, object::pool<mm::Mem>& memory,
                   object::pool<irq::Irq>& irqs, mm::Pmm& pmm, const BootInfo& boot,
                   const time::Clock& clock) noexcept -> bool {
    (void) plic.emplace();
    const mm::Extent extent{.object = mm::ObjectRange{0, 1},
                            .physical = {mm::Page{VirtUartBase / mm::page_size}, 1},
                            .perms = mm::Perms::of(mm::Perm::Read, mm::Perm::Write)};
    auto mem = memory.create(pmm, mm::page_size, mm::PhysCfg{{&extent, 1}, {}});
    auto irq = irqs.create(irq::Line{plic->routes(), VirtUartIrq});
    if (!mem || !irq)
        return false;
    (void) uart.emplace(std::move(mem).value().publish(), std::move(irq).value().publish());
    if (!boot.iommu)
        return true;
    auto& bus = pci.emplace(pmm, mm::DirectBegin + boot.iommu->base().base().raw());
    if (!bus.iommu.start(pmm))
        return false;
    const auto timeout = clock.duration_from_nanoseconds(100'000'000);
    if (!timeout)
        return false;
    const auto begin = clock.now().ticks();
    // Startup only. Runtime waits remain retained IOSpace work.
    for (;;) {
        const auto status = bus.iommu.initialize();
        if (status == Iommu::Step::Failed)
            return false;
        if (status == Iommu::Step::Complete)
            break;
        if (clock.now().ticks() - begin >= timeout->ticks())
            return false;
    }
    auto found = [&](PciFn&& fn) noexcept {
        // This backend supports isolated INTx leases. Shared pins require a
        // shared-source driver protocol and must not silently alias ownership.
        for (const auto& d : bus.iommu.devices())
            if (d.irq().id == fn.irq_source())
                return false;
        auto pending = bus.nodes.owner().group();
        auto page = pending.allocate();
        if (!page)
            return false;
        const u32 id = fn.irq_source();
        static_assert(sizeof(PciDma) <= mm::page_size);
        auto* node =
            libk::construct_at(reinterpret_cast<PciDma*>(pending.bytes(page.value())),
                               std::move(fn), bus.iommu, clock, irq::Line{plic->routes(), id});
        auto device = devices.create(*node);
        if (!device) {
            libk::destroy_at(node);
            return false;
        }
        node->device = std::move(device).value().publish();
        bus.nodes.append(std::move(pending));
        console::print<"io: isolated PCI function ready requester={:#x}\n">(
            node->device->requester());
        return true;
    };
    const auto scan = PciFn::scan(PciFn::Found::bind(found));
    if (!scan && scan.error() != PciError::Absent)
        return false;
    return bus.faults.connect();
}

auto virt_caps(BootCaps publish) noexcept -> bool {
    auto emit = [&](BootCap entry, auto& object, cap::View view) noexcept {
        auto ref = object.erase();
        return ref && publish(entry, std::move(ref).value(), view);
    };
    const auto rights = [](auto... extra) noexcept {
        return cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect,
                               cap::Right::Revoke, extra...);
    };
    if (!emit({0, 0x55415254, 1, 0, OBJECT_KIND_MEMORY, 0, 0, "uart.memory"}, uart->memory,
              {rights(cap::Right::Map),
               cap::MemLimit{mm::ObjectRange{0, 1},
                             mm::Perms::of(mm::Perm::Read, mm::Perm::Write)}}) ||
        !emit({0, 0x55415254, 1, 0, OBJECT_KIND_IRQ, 0, 0, "uart.irq"}, uart->irq,
              {rights(cap::Right::Route, cap::Right::Observe, cap::Right::Ack, cap::Right::Close),
               cap::IrqRoute{VirtUartIrq, uart->irq->source().level}}))
        return false;
    if (pci)
        for (const auto& d : pci->iommu.devices()) {
            BootCap entry{0, 0x50434944, 1, 0, OBJECT_KIND_DEVICE, 0, 0, "pci."};
            constexpr char hex[] = "0123456789abcdef";
            const u16 id = d.device->requester();
            for (usize i = 0; i < 4; ++i)
                entry.name[4 + i] = hex[(id >> (12 - i * 4)) & 15];
            if (!emit(entry, d.device, {rights(cap::Right::Connect), {}}))
                return false;
        }
    return true;
}

auto virt_mmio(const BootInfo& boot) noexcept -> std::array<mm::Region, 5> {
    return {{
        {{mm::Page{VirtUartBase / mm::page_size}, 1}, mm::Region::Kind::Mmio},
        {{mm::Page{VirtPlicBase / mm::page_size}, VirtPlicSize / mm::page_size},
         mm::Region::Kind::Mmio},
        {{mm::Page{VirtPciEcam / mm::page_size}, 256}, mm::Region::Kind::Mmio},
        {{mm::Page{VirtPciMmio / mm::page_size}, 256}, mm::Region::Kind::Mmio},
        {boot.iommu.value_or(mm::Pages{}), mm::Region::Kind::Mmio},
    }};
}
