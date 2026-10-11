#include <algorithm>
#include <expected>
#include <utility>
#include <boot/info.hpp>
#include "dma.hpp"
#include <platform/plic.hpp>
#include <libk/manual_lifetime.hpp>
#include <mm/table.hpp>
#include <libk/pci.hpp>
#include <libk/fdt.hpp>
#include <uapi/start.h>
#include <limits>

struct FwHw {
    u32 uart_node{}, pci_node{};
    BootSpan plic, uart, iommu;
    usize ctx{};
    u32 nirq{}, uart_irq{}, iommu_irq{};
    FwPci pci{};
};

static auto read_cells(libk::byte_reader& r, u32 n, u64& value) noexcept -> bool {
    if (n > 2) return false;
    value = 0;
    while (n--) {
        u32 word;
        if (!r.read_be(word)) return false;
        value = (value << 32) | word;
    }
    return true;
}
static auto cell_count(const libk::fdt& t, u32 node, std::string_view key, u32 fallback) noexcept -> u32 {
    return t.property(node, key) ? t.number(node, key).value_or(UINT32_MAX) : fallback;
}

// RAM contributes whole frames; reservations protect every touched frame.
// Both use the parent bus's address/size encoding, without a second parser state.
static auto read_memory(const libk::fdt& t, mm::RegionList& memory) noexcept -> bool {
    const u32 root = t.root();
    const auto addr = cell_count(t, root, "#address-cells", 2);
    const auto size = cell_count(t, root, "#size-cells", 2);
    auto visit = [&](u32 parent, bool reserved) {
        const auto ac = cell_count(t, parent, "#address-cells", addr);
        const auto sc = cell_count(t, parent, "#size-cells", size);
        if (!ac || ac > 2 || !sc || sc > 2) return false;
        for (auto n = t.first_child(parent); n; n = t.next_sibling(*n)) {
            const auto name = t.node_name(*n);
            if (!reserved && name != "memory" && !name.starts_with("memory@")) continue;
            const auto reg = t.property(*n, "reg");
            if (!reg) continue;
            if (reg->empty()) return false;
            libk::byte_reader r{reg->data(), reg->size()};
            while (r.remaining()) {
                u64 pa, bytes;
                if (!read_cells(r, ac, pa) || !read_cells(r, sc, bytes)) return false;
                const auto pages = reserved ? mm::Pages::covering_bytes(mm::pa_t{pa}, bytes)
                                            : mm::Pages::contained_bytes(mm::pa_t{pa}, bytes);
                if (!pages || !memory.try_emplace_back(mm::Region{*pages,
                    reserved ? mm::Region::Kind::Firmware : mm::Region::Kind::Ram})) return false;
            }
        }
        return true;
    };
    const auto reserved = t.child(root, "reserved-memory");
    return visit(root, false) && (!reserved || visit(*reserved, true));
}

static auto read_module(const libk::fdt& t) noexcept -> std::expected<std::optional<BootSpan>, libk::fdt::error> {
    const auto chosen = t.child(t.root(), "chosen");
    if (!chosen) return std::nullopt;
    const auto begin = t.property(*chosen, "linux,initrd-start");
    const auto end = t.property(*chosen, "linux,initrd-end");
    if (!begin && !end) return std::nullopt;
    const auto ac = cell_count(t, t.root(), "#address-cells", 2);
    u64 lo, hi;
    auto address = [&](const auto& bytes, u64& value) {
        if (!bytes || bytes->size() != ac * 4) return false;
        libk::byte_reader r{bytes->data(), bytes->size()};
        return read_cells(r, ac, value);
    };
    if (!address(begin, lo) || !address(end, hi) || lo >= hi || !BootSpan{lo, hi - lo})
        return std::unexpected(libk::fdt::error::invalid_structure);
    return BootSpan{lo, hi - lo};
}

// Firmware status is boot input, not a second runtime CPU state machine.
static auto parse_cpus(const libk::fdt& tree, CpuHwId boot,
                       libk::inplace_vector<CpuHwId, MaxCpus>& out) noexcept -> bool {
    const auto node = tree.child(tree.root(), "cpus");
    if (!node) return false;
    u32 cells{}, sizes{};
    const auto address = tree.property(*node, "#address-cells");
    const auto size = tree.property(*node, "#size-cells");
    if (!address
        || !libk::fdt::read_u32(*address, cells) || cells < 1 || cells > 2
        || !size
        || !libk::fdt::read_u32(*size, sizes) || sizes != 0) return false;
    for (auto cpu = tree.first_child(*node); cpu; cpu = tree.next_sibling(*cpu)) {
        const auto name = tree.node_name(*cpu);
        if (name != "cpu" && !name.starts_with("cpu@")) continue;
        const auto type = tree.property(*cpu, "device_type");
        const auto reg = tree.property(*cpu, "reg");
        if (!type
            || libk::fdt::first_string(*type) != "cpu"
            || !reg
            || reg->size() != cells * sizeof(u32)) return false;
        libk::byte_reader reader{reg->data(), reg->size()};
        u64 raw{};
        for (u32 i = 0; i < cells; ++i) {
            u32 cell{};
            if (!reader.read_be(cell)) return false;
            raw = (raw << 32) | cell;
        }
        if (raw > std::numeric_limits<usize>::max()) return false;
        const CpuHwId id{static_cast<usize>(raw)};
        if (const auto status = tree.property(*cpu, "status")) {
            const auto text = libk::fdt::first_string(*status);
            if (!text) return false;
            if (*text != "okay" && *text != "ok") {
                if (*text != "disabled" && *text != "fail" && !text->starts_with("fail-")) return false;
                continue;
            }
        }
        for (const auto prev : out) if (prev == id) return false;
        if (!out.try_push_back(id)) return false;
    }
    const auto first = std::find(out.begin(), out.end(), boot);
    if (first == out.end()) return false;
    std::iter_swap(out.begin(), first);
    return true;
}

static auto translate(const libk::fdt& t, u32 bus, BootSpan range) noexcept -> std::optional<BootSpan> {
    while (const auto parent = t.parent(bus)) {
        const auto ranges = t.property(bus, "ranges");
        if (!ranges) return {};
        if (!ranges->empty()) {
            libk::byte_reader r{ranges->data(), ranges->size()};
            bool found = false;
            while (r.remaining()) {
                u64 child, pa, size;
                if (!read_cells(r, cell_count(t, bus, "#address-cells", 2), child) ||
                    !read_cells(r, cell_count(t, *parent, "#address-cells", 2), pa) ||
                    !read_cells(r, cell_count(t, bus, "#size-cells", 1), size)) return {};
                if (range.pa < child || range.pa - child > size || range.size > size - (range.pa - child)) continue;
                if (pa > UINT64_MAX - (range.pa - child)) return {};
                range.pa = pa + (range.pa - child);
                found = true;
                break;
            }
            if (!found) return {};
        }
        bus = *parent;
    }
    if (!range.size || range.pa >= mm::DirectSize || range.size > mm::DirectSize - range.pa) return {};
    return range;
}
static auto register_range(const libk::fdt& t, u32 node) noexcept -> std::optional<BootSpan> {
    const auto parent = t.parent(node);
    const auto reg = t.property(node, "reg");
    if (!parent || !reg) return {};
    libk::byte_reader r{reg->data(), reg->size()};
    u64 pa, size;
    if (!read_cells(r, cell_count(t, *parent, "#address-cells", 2), pa) ||
        !read_cells(r, cell_count(t, *parent, "#size-cells", 1), size) || r.remaining()) return {};
    return translate(t, *parent, {pa, size});
}
static auto wired_irq(const libk::fdt& t, u32 node, u32 plic, u32 count) noexcept -> std::optional<u32> {
    std::optional<u32> parent;
    for (auto n = std::optional{node}; n && !parent; n = t.parent(*n)) parent = t.number(*n, "interrupt-parent");
    const auto p = t.property(node, "interrupts");
    if (parent != plic || !p) return {};
    // Vector zero is sufficient; IOMMU directs its fault queue to that vector.
    libk::byte_reader r{p->data(), p->size()};
    u32 id;
    return r.read_be(id) && id && id <= count ? std::optional{id} : std::nullopt;
}
auto FwPci::irq(u16 rid, u8 pin) const noexcept -> std::optional<u32> {
    const std::array<u32, 4> key{u32{rid} << 8, 0, 0, pin};
    libk::byte_reader r{routes.data(), routes.size()};
    while (r.remaining()) {
        bool match = true;
        u32 word, parent, id;
        for (usize i = 0; i < key.size(); ++i) {
            if (!r.read_be(word)) return {};
            match &= (word & mask[i]) == (key[i] & mask[i]);
        }
        if (!r.read_be(parent) || !r.read_be(id)) return {};
        if (match) return id;
    }
    return {};
}
auto FwPci::device(u16 rid) const noexcept -> std::optional<u16> {
    libk::byte_reader r{devices.data(), devices.size()};
    while (r.remaining()) {
        u32 first, parent, output, count;
        if (!r.read_be(first) || !r.read_be(parent) || !r.read_be(output) || !r.read_be(count)) return {};
        if (parent == iommu && rid >= first && u32{rid} - first < count && output <= 255 && rid - first <= 255 - output)
            return static_cast<u16>(output + rid - first);
    }
    return {};
}
static auto read_hw(const libk::fdt& t, CpuHwId hart, FwHw& hw) noexcept -> bool {
    const auto plic = t.find("sifive,plic-1.0.0");
    const auto chosen = t.child(t.root(), "chosen");
    const auto stdout_path = chosen ? t.property(*chosen, "stdout-path") : std::nullopt;
    const auto name = stdout_path ? libk::fdt::first_string(*stdout_path) : std::nullopt;
    const auto uart = name ? t.path(*name) : std::nullopt;
    if (!plic || !uart || !t.matches(*uart, "ns16550a") ||
        cell_count(t, *uart, "reg-shift", 0) != 0 || cell_count(t, *uart, "reg-io-width", 1) != 1 ||
        t.number(*plic, "#interrupt-cells") != 1 || t.number(*plic, "#address-cells") != 0) return false;
    const auto plic_id = t.number(*plic, "phandle");
    const auto nirq = t.number(*plic, "riscv,ndev");
    const auto pr = register_range(t, *plic), ur = register_range(t, *uart);
    const auto contexts = t.property(*plic, "interrupts-extended");
    if (!plic_id || !nirq || !*nirq || *nirq >= 1024 || !pr || !ur || !contexts) return false;
    libk::byte_reader r{contexts->data(), contexts->size()};
    std::optional<usize> ctx;
    for (usize i = 0; r.remaining(); ++i) {
        u32 ref, irq;
        if (!r.read_be(ref) || !r.read_be(irq)) return false;
        const auto intc = t.phandle(ref);
        const auto cpu = intc ? t.parent(*intc) : std::nullopt;
        if (!intc || !cpu || !t.matches(*intc, "riscv,cpu-intc") || t.number(*intc, "#interrupt-cells") != 1) return false;
        const auto id = t.property(*cpu, "reg");
        u64 value;
        if (!id || id->empty() || id->size() > 8) return false;
        libk::byte_reader cr{id->data(), id->size()};
        if (!read_cells(cr, id->size() / 4, value)) return false;
        if (value == hart.raw && irq == 9) { if (ctx) return false; ctx = i; }
    }
    const auto uart_irq = wired_irq(t, *uart, *plic_id, *nirq);
    if (!ctx || !uart_irq || *ctx > (UINT64_MAX - 0x200008) / 0x1000 ||
        pr->size < 0x200008 + *ctx * 0x1000) return false;
    hw.uart_node = *uart;
    hw.plic = *pr; hw.uart = *ur; hw.ctx = *ctx; hw.nirq = *nirq; hw.uart_irq = *uart_irq;
    const auto pci = t.find("pci-host-ecam-generic");
    const auto iommu = t.find("riscv,iommu");
    if (!pci || !iommu) return true; // No isolated PCI bus is exposed without its isolation backend.
    const auto ir = register_range(t, *iommu), ecam = register_range(t, *pci);
    const auto iommu_id = t.number(*iommu, "phandle");
    const auto iommu_irq = wired_irq(t, *iommu, *plic_id, *nirq);
    const auto parent = t.parent(*pci);
    const auto ranges = t.property(*pci, "ranges"), buses = t.property(*pci, "bus-range");
    const auto routes = t.property(*pci, "interrupt-map"), mask = t.property(*pci, "interrupt-map-mask");
    const auto devices = t.property(*pci, "iommu-map");
    if (!ir || ir->size < mm::page_size || !ecam || ecam->size < (1 << 20) || !iommu_id || !iommu_irq || !parent ||
        !ranges || !buses || !routes || !mask || !devices ||
        t.number(*iommu, "#iommu-cells") != 1 || cell_count(t, *pci, "#address-cells", 3) != 3 ||
        cell_count(t, *pci, "#size-cells", 2) != 2 || t.number(*pci, "#interrupt-cells") != 1 ||
        cell_count(t, *pci, "iommu-map-mask", 0xffff) != 0xffff ||
        buses->size() != 8 || routes->size() % 24 || mask->size() != 16 || devices->size() % 16) return false;
    libk::byte_reader br{buses->data(), buses->size()};
    u32 first, last;
    if (!br.read_be(first) || !br.read_be(last) || first != 0 || last > 255) return false;
    // Current DMA directory and user enumerator cover root-bus functions only.
    hw.pci_node = *pci;
    hw.pci.cfg = {ecam->pa, 1 << 20};
    libk::byte_reader rr{ranges->data(), ranges->size()};
    while (rr.remaining()) {
        u32 flags; u64 bus, pa, size;
        if (!rr.read_be(flags) || !read_cells(rr, 2, bus) ||
            !read_cells(rr, cell_count(t, *parent, "#address-cells", 2), pa) || !read_cells(rr, 2, size)) return false;
        if ((flags & 0x43000000) != 0x02000000 || hw.pci.window.size) continue;
        const auto window = translate(t, *parent, {pa, size});
        // CPU and PCI addresses currently share the public BAR placement metadata.
        if (!window || window->pa != bus) return false;
        hw.pci.window = *window;
    }
    if (!hw.pci.window.size) return false;
    libk::byte_reader mr{mask->data(), mask->size()};
    for (auto& word : hw.pci.mask) if (!mr.read_be(word)) return false;
    libk::byte_reader route{routes->data(), routes->size()};
    while (route.remaining()) {
        u64 addr, pin, ref, id;
        if (!read_cells(route, 2, addr) || !read_cells(route, 2, pin) ||
            !read_cells(route, 1, ref) || !read_cells(route, 1, id) || ref != *plic_id || !id || id > *nirq) return false;
    }
    hw.pci.routes = *routes; hw.pci.devices = *devices; hw.pci.iommu = *iommu_id;
    hw.iommu = *ir; hw.iommu_irq = *iommu_irq;
    return true;
}

static auto read_fdt(
    BootInfo& info,
    mm::RegionList& regions,
    CpuHwId boot_cpu,
    mm::pa_t fdt_physical,
    const void* fdt_pointer, FwHw& hw) noexcept -> std::expected<void, libk::fdt::error> {
    info.fw_format = 0;
    info.firmware = {};
    info.module.reset();
    info.cpus.clear();
    info.timebase_frequency = 0;
    hw = {};
    regions.clear();

    auto opened = libk::fdt::open(fdt_pointer);
    if (!opened) {
        return std::unexpected(libk::fdt::error::invalid_header);
    }
    const libk::fdt& tree = opened.value();

    if (!parse_cpus(tree, boot_cpu, info.cpus)) {
        return std::unexpected(libk::fdt::error::invalid_structure);
    }
    auto& memory = regions;
    const auto cpus = tree.child(tree.root(), "cpus");
    const auto timebase = cpus ? tree.number(*cpus, "timebase-frequency") : std::nullopt;
    if (!timebase || !*timebase) {
        return std::unexpected(libk::fdt::error::invalid_structure);
    }
    info.timebase_frequency = *timebase;

    if (!read_memory(tree, memory)) {
        return std::unexpected(libk::fdt::error::invalid_structure);
    }
    if (!read_hw(tree, boot_cpu, hw)) return std::unexpected(libk::fdt::error::invalid_structure);

    const bool reservations_valid = tree.for_each_reservation(
        [&memory](uint64_t address, uint64_t size) {
            const auto range = mm::Pages::covering_bytes(mm::pa_t{address}, size);
            return range && memory.try_emplace_back(mm::Region{*range, mm::Region::Kind::Firmware});
        });
    if (!reservations_valid) {
        return std::unexpected(libk::fdt::error::invalid_structure);
    }

    auto module = read_module(tree);
    if (!module) {
        return std::unexpected(module.error());
    }
    if (module.value()
        && !memory.try_emplace_back(mm::Region{module.value()->pages(), mm::Region::Kind::Boot})) {
        return std::unexpected(libk::fdt::error::invalid_structure);
    }

    const auto fdt_pages = mm::Pages::covering_bytes(
        fdt_physical,
        tree.size());
    if (!fdt_pages
        || !memory.try_emplace_back(mm::Region{*fdt_pages, mm::Region::Kind::Boot})) {
        return std::unexpected(libk::fdt::error::invalid_header);
    }

    info.fw_format = FW_FDT;
    info.firmware = {fdt_physical.raw(), tree.size()};
    info.module = std::move(module).value();

    return {};
}

static constinit libk::manual_lifetime<Plic> plic{};
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
        const pci::cfg<arch::io_fence> cfg{mm::DirectBegin + pci.cfg.pa + (rid << 12)};
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
static libk::manual_lifetime<PciBus> bus_storage{};

static auto start_hw(mm::Pmm& pmm, const time::Clock& clock, HwEmit emit) noexcept -> ExtIrq {
    auto& controller = plic.emplace();
    controller.start(mm::DirectBegin + wiring.plic.pa, wiring.ctx, wiring.nirq);
    const auto ext_irq = ExtIrq::bind<&Plic::dispatch>(controller);
    const auto uart = wiring.uart.pages();
    if (!uart.valid()) return {};
    const io::Reg reg{uart.base().base().raw(), uart.byte_size(),
                      mm::Perms::of(mm::Perm::Read, mm::Perm::Write)};
    if (!emit(wiring.uart_node, 0, reg) ||
        !emit(wiring.uart_node, 0,
            irq::Line{plic->routes(), wiring.uart_irq}))
        return {};
    if (!wiring.iommu.size) return ext_irq;
    const auto rw = mm::Perms::of(mm::Perm::Read, mm::Perm::Write);
    if (!emit(wiring.pci_node, 0,
            io::Reg{wiring.pci.cfg.pa, wiring.pci.cfg.size, rw}) ||
        !emit(wiring.pci_node, 1,
            io::Reg{wiring.pci.window.pa, wiring.pci.window.size, rw})) return {};
    auto& bus = bus_storage.emplace(pmm, wiring, clock);
    if (!bus.iommu.start(pmm))
        return {};
    const auto timeout = clock.ticks<100, time::ms>();
    if (!timeout)
        return {};
    const auto begin = clock.rd_ticks();
    // Startup only. Runtime waits remain retained IOSpace work.
    for (;;) {
        const auto status = bus.iommu.initialize();
        if (status == Iommu::Step::Failed)
            return {};
        if (status == Iommu::Step::Complete)
            break;
        if (clock.rd_ticks() - begin >= *timeout)
            return {};
    }
    if (!emit(wiring.pci_node, 0,
            static_cast<io::Bus*>(&bus))) return {};
    return bus.faults.connect() ? ext_irq : ExtIrq{};
}

static auto read_hw(BootInfo& boot, mm::RegionList& memory, CpuHwId hart, mm::pa_t pa) noexcept -> bool {
    if (!read_fdt(boot, memory, hart, pa,
                  reinterpret_cast<const void*>(mm::DirectBegin + pa.raw()), wiring))
        return false;
    for (const auto r : {wiring.uart, wiring.plic, wiring.pci.cfg, wiring.pci.window, wiring.iommu}) {
        if (!r.size) continue;
        const auto pages = r.pages();
        if (!pages.valid() || !memory.try_emplace_back(mm::Region{pages, mm::Region::Kind::Mmio})) return false;
    }
    return true;
}

extern "C" [[noreturn]] void boot_enter(usize hart, usize pa) noexcept {
    boot_start(CpuHwId{hart}, mm::pa_t{pa}, read_hw, start_hw);
}
