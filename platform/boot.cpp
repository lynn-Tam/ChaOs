#include <expected>
#include <utility>
#include <platform/boot.hpp>
#include "fdt.hpp"
#include "dma.hpp"
#include <platform/plic.hpp>
#include <console.hpp>
#include <libk/manual_lifetime.hpp>
#include <mm/table.hpp>
#include <libk/pci.hpp>

static constinit libk::ManualLifetime<Plic> plic{};
static FwHw wiring{};

// The IOMMU owns its controller storage and the identities behind it. Nodes
// are funded from PMM, independent of interrupt numbers or a fixed registry.
struct PciBus final : io::Bus {
    PciBus(mm::Pmm& pmm, const FwHw& hw, const time::Clock& clock) noexcept
        : Bus(hw.pci.cfg.size / mm::page_size), iommu(mm::DirectBegin + hw.iommu.pa), nodes(pmm.group()), clock(clock), pci(hw.pci),
          faults({plic->routes(), hw.iommu_irq},
                 irq::Route::Handler::bind<&Iommu::interrupt>(iommu)) {}
    auto acquire(usize rid, io::Hw::Stop stop) noexcept -> std::expected<io::Hw*, io::BindErr> override {
        sync::Lock lock{gate};
        for (auto* d = iommu.devices(); d; d = d->next)
            if (d->requester() == rid) return d->acquire(stop);
        const pci::Cfg<arch::io_fence> cfg{mm::DirectBegin + pci.cfg.pa + (rid << 12)};
        const auto pin = cfg.read<u8>(0x3d);
        if (pin == 0 || pin > 4) return std::unexpected(io::BindErr::Unavailable);
        const auto id = pci.irq(static_cast<u16>(rid), pin);
        const auto did = pci.device(static_cast<u16>(rid));
        if (!id || !did) return std::unexpected(io::BindErr::Unavailable);
        // Shared wired lines need a demultiplexer; reject instead of aliasing owners.
        for (auto* d = iommu.devices(); d; d = d->next)
            if (d->irq().id == *id || d->device_id() == *did) return std::unexpected(io::BindErr::Busy);
        auto pending = nodes.owner().group();
        auto page = pending.allocate();
        if (!page) return std::unexpected(io::BindErr::NoMemory);
        static_assert(sizeof(PciDma) <= mm::page_size);
        auto* d = libk::construct_at(reinterpret_cast<PciDma*>(pending.bytes(*page)),
            static_cast<u16>(rid), *did, pci, iommu, clock, irq::Line{plic->routes(), *id});
        auto bound = d->acquire(stop);
        if (!bound) { libk::destroy_at(d); return bound; }
        nodes.append(std::move(pending));
        iommu.add(*d);
        return bound;
    }
    Iommu iommu;
    mm::PageGroup nodes;
    const time::Clock& clock;
    const FwPci& pci;
    irq::Route faults;
    sync::Spin gate{};
};
static libk::ManualLifetime<PciBus> bus_storage{};

auto platform_start(mm::Pmm& pmm, const time::Clock& clock,
                    BootHw& hw) noexcept -> bool {
    auto& controller = plic.emplace();
    controller.start(mm::DirectBegin + wiring.plic.pa, wiring.ctx, wiring.nirq);
    hw.ext_irq = libk::delegate<void() noexcept>::bind<&Plic::dispatch>(controller);
    const auto uart = wiring.uart.pages();
    if (!uart) return false;
    const io::Reg reg{uart->base().base().raw(), uart->byte_size(),
                      mm::Perms::of(mm::Perm::Read, mm::Perm::Write)};
    if (!hw.resources.try_emplace_back(
            BootCap{0, 0x55415254, 1, 0, OBJECT_KIND_MEMORY, 0, 0, "uart.memory", 0, 0}, reg) ||
        !hw.resources.try_emplace_back(
            BootCap{0, 0x55415254, 1, 0, OBJECT_KIND_IRQ, 0, 0, "uart.irq", 0, 0},
            irq::Line{plic->routes(), wiring.uart_irq}))
        return false;
    if (!wiring.iommu.size) return true;
    const auto rw = mm::Perms::of(mm::Perm::Read, mm::Perm::Write);
    if (!hw.resources.try_emplace_back(
            BootCap{0, 0x50434920, 1, 0, OBJECT_KIND_MEMORY, 0, 0, "pci.cfg", 0, 0},
            io::Reg{wiring.pci.cfg.pa, wiring.pci.cfg.size, rw}) ||
        !hw.resources.try_emplace_back(
            BootCap{0, 0x50434920, 1, 0, OBJECT_KIND_MEMORY, 0, 0, "pci.mmio", 0, 0},
            io::Reg{wiring.pci.window.pa, wiring.pci.window.size, rw})) return false;
    auto& bus = bus_storage.emplace(pmm, wiring, clock);
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
    if (!hw.resources.try_emplace_back(
            BootCap{0, 0x50434920, 1, 0, OBJECT_KIND_IO_HOST, 0, 0, "pci.host", 0, 0},
            static_cast<io::Bus*>(&bus))) return false;
    return bus.faults.connect();
}

auto platform_boot(BootInfo& boot, mm::RegionList& memory, CpuHwId hart, mm::Phys pa) noexcept -> bool {
    if (!read_fdt(boot, memory, hart, pa,
                  reinterpret_cast<const void*>(mm::DirectBegin + pa.raw()), wiring))
        return false;
    for (const auto r : {wiring.uart, wiring.plic, wiring.pci.cfg, wiring.pci.window, wiring.iommu}) {
        if (!r.size) continue;
        const auto pages = r.pages();
        if (!pages || !memory.try_emplace_back(mm::Region{*pages, mm::Region::Kind::Mmio})) return false;
    }
    return true;
}
