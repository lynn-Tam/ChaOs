#include <algorithm>
#include <boot/info.hpp>
#include <libk/fdt.hpp>
#include <uapi/start.h>
#include <limits>

// RAM contributes whole frames; reservations protect every touched frame.
// Both use the parent bus's address/size encoding, without a second parser state.
static auto read_memory(const libk::fdt& t, mm::RegionList& memory, mm::RegionList& protected_ranges) noexcept -> bool {
    const u32 root = t.root();
    const auto addr = t.cell_count(root, "#address-cells", 2);
    const auto size = t.cell_count(root, "#size-cells", 2);
    auto visit = [&](u32 parent, bool reserved) {
        const auto ac = t.cell_count(parent, "#address-cells", addr);
        const auto sc = t.cell_count(parent, "#size-cells", size);
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
                if (!libk::fdt::read_cells(r, ac, pa) || !libk::fdt::read_cells(r, sc, bytes)) return false;
                const auto pages = reserved ? mm::Pages::covering_bytes(mm::pa_t{pa}, bytes)
                                            : mm::Pages::contained_bytes(mm::pa_t{pa}, bytes);
                if (!pages || !memory.try_emplace_back(mm::Region{*pages,
                    reserved ? mm::Region::Kind::Firmware : mm::Region::Kind::Ram})) return false;
                if (reserved && !protected_ranges.try_emplace_back(
                        mm::Region{*pages, mm::Region::Kind::Firmware})) return false;
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
    const auto ac = t.cell_count(t.root(), "#address-cells", 2);
    u64 lo, hi;
    auto address = [&](const auto& bytes, u64& value) {
        if (!bytes || bytes->size() != ac * 4) return false;
        libk::byte_reader r{bytes->data(), bytes->size()};
        return libk::fdt::read_cells(r, ac, value);
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

static auto read_fdt(
    BootInfo& info,
    mm::RegionList& regions,
    CpuHwId boot_cpu,
    mm::pa_t fdt_physical,
    const void* fdt_pointer) noexcept -> std::expected<void, libk::fdt::error> {
    info.fw_format = 0;
    info.firmware = {};
    info.module.reset();
    info.cpus.clear();
    info.timebase_frequency = 0;
    info.reserved.clear();
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

    if (!read_memory(tree, memory, info.reserved)) {
        return std::unexpected(libk::fdt::error::invalid_structure);
    }

    const bool reservations_valid = tree.for_each_reservation(
        [&](uint64_t address, uint64_t size) {
            const auto range = mm::Pages::covering_bytes(mm::pa_t{address}, size);
            return range && memory.try_emplace_back(mm::Region{*range, mm::Region::Kind::Firmware})
                && info.reserved.try_emplace_back(mm::Region{*range, mm::Region::Kind::Firmware});
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

static auto read(BootInfo& info, mm::RegionList& memory, CpuHwId hart, mm::pa_t pa) noexcept -> bool {
    return bool(read_fdt(info, memory, hart, pa,
                        reinterpret_cast<const void*>(mm::DirectBegin + pa.raw())));
}
extern "C" [[noreturn]] void boot_enter(usize hart, usize pa) noexcept {
    boot_start(CpuHwId{hart}, mm::pa_t{pa}, read);
}
