#include <expected>
#include <utility>
#include "board.hpp"
#include "dma.hpp"
#include <arch/riscv64/irq/plic.hpp>
#include <arch/riscv64/cpu/csr.hpp>
#include <console.hpp>
#include <arch/trap.hpp>
#include <libk/manual_lifetime.hpp>
#include <mm/table.hpp>

static constinit libk::ManualLifetime<Plic> plic{};

// The IOMMU owns its controller storage and the identities behind it. Nodes
// are funded from PMM, independent of interrupt numbers or a fixed registry.
struct PciBus final {
    struct Node final {
        Node(PciFn&& fn, arch::Iommu& iommu, const time::Clock& clock, irq::Line line) noexcept
            : hw(std::move(fn), iommu, clock, line) {}
        PciDma hw;
        object::ref<io::Device> device{};
        Node* next{};
    };
    PciBus(mm::Pmm& pmm, usize base) noexcept
        : iommu(base), nodes(pmm.group()),
          faults({plic->routes(), VirtIommuIrq}, irq::Route::Handler::bind<&PciBus::fault>(*this)) {}
    auto fault() noexcept -> bool {
        const bool healthy = iommu.handle_fault_irq() == arch::IoStatus::Complete;
        const bool all = !healthy || iommu.fault_overflow();
        for (auto* n = head; n; n = n->next)
            if (all || iommu.fault_pending(n->device->requester())) n->device->signal_fault();
        return healthy;
    }
    arch::Iommu iommu;
    mm::PageGroup nodes;
    irq::Route faults;
    Node* head{};
    Node** tail{&head};
    usize count{};
};
static libk::ManualLifetime<PciBus> pci{};

auto virt_uart_irq() noexcept -> irq::Line { return {plic->routes(), VirtUartIrq}; }
void arch::start_irqs(usize hart) noexcept {
    plic->start(mm::DirectBegin + VirtPlicBase, hart * 2 + 1);
    arch::riscv64::Sie::enable_external();
}
void arch::external_irq() noexcept { plic->dispatch(); }

auto virt_io_start(io::objects& objects, mm::Pmm& pmm,
    const BootInfo& boot, const time::Clock& clock) noexcept -> bool {
    (void)plic.emplace();
    if (!boot.iommu) return true;
    auto& bus = pci.emplace(pmm, mm::DirectBegin + boot.iommu->base().base().raw());
    if (!bus.iommu.start(pmm)) return false;
    const auto timeout = clock.duration_from_nanoseconds(100'000'000);
    if (!timeout) return false;
    const auto begin = clock.now().ticks();
    // Startup only. Runtime waits remain retained IOSpace work.
    for (;;) {
        const auto status = bus.iommu.initialize();
        if (status == arch::IoStatus::Failed) return false;
        if (status == arch::IoStatus::Complete) break;
        if (clock.now().ticks() - begin >= timeout->ticks()) return false;
    }
    auto found = [&](PciFn&& fn) noexcept {
        // This backend supports isolated INTx leases. Shared pins require a
        // shared-source driver protocol and must not silently alias ownership.
        for (auto* n = bus.head; n; n = n->next)
            if (n->hw.irq().id == fn.irq_source()) return false;
        auto pending = bus.nodes.owner().group();
        auto page = pending.allocate();
        if (!page) return false;
        const u32 id = fn.irq_source();
        auto* node = libk::construct_at(reinterpret_cast<PciBus::Node*>(pending.bytes(page.value())),
            std::move(fn), bus.iommu, clock, irq::Line{plic->routes(), id});
        auto device = objects.devices.create(node->hw);
        if (!device) { libk::destroy_at(node); return false; }
        node->device = std::move(device).value().publish();
        bus.nodes.append(std::move(pending));
        *bus.tail = node;
        bus.tail = &node->next;
        ++bus.count;
        console::print<"io: isolated PCI function ready requester={:#x}\n">(node->device->requester());
        return true;
    };
    const auto scan = PciFn::scan(PciFn::Found::bind(found));
    if (!scan && scan.error() != PciError::Absent) return false;
    return bus.faults.connect();
}

auto virt_device_count() noexcept -> usize { return pci ? pci->count : 0; }
auto virt_device(usize index) noexcept -> io::Device& {
    libk_assert(pci && index < pci->count);
    auto* n = pci->head;
    while (index--) n = n->next;
    return n->device.get();
}
auto virt_device_ref(usize index) noexcept -> std::expected<object::ref<>, object::error> {
    libk_assert(pci && index < pci->count);
    auto* n = pci->head;
    while (index--) n = n->next;
    return n->device.erase();
}

auto virt_mmio(const BootInfo& boot) noexcept -> std::array<mm::Region, 5> {
    return {{
        {{mm::Page{VirtUartBase / mm::page_size}, 1}, mm::Region::Kind::Mmio, mm::CpuAttr::Native},
        {{mm::Page{VirtPlicBase / mm::page_size}, VirtPlicSize / mm::page_size}, mm::Region::Kind::Mmio, mm::CpuAttr::Native},
        {{mm::Page{VirtPciEcam / mm::page_size}, 256}, mm::Region::Kind::Mmio, mm::CpuAttr::Native},
        {{mm::Page{VirtPciMmio / mm::page_size}, 256}, mm::Region::Kind::Mmio, mm::CpuAttr::Native},
        {boot.iommu.value_or(mm::Pages{}), mm::Region::Kind::Mmio, mm::CpuAttr::Native},
    }};
}
