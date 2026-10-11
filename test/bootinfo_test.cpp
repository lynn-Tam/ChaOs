#include <test/test.hpp>
#include <algorithm>

#include <libk/fdt.hpp>
#include <utility>
#include <boot/info.hpp>
#include <mm/pmm.hpp>

namespace {

bool test_default_bootinfo_is_cleared(const TestContext&) noexcept {
    BootInfo boot{};
    return !boot.firmware
        && boot.firmware.pa == 0
        && boot.firmware.size == 0
        && !boot.firmware.pages().valid();
}

bool test_boot_map_is_valid_and_non_overlapping(const TestContext& ctx) noexcept {
    const auto map = ctx.memory.regions();
    if (map.empty()) {
        return false;
    }
    for (size_t i = 0; i < map.size(); ++i) {
        if (!map[i].valid()) {
            return false;
        }
        for (size_t j = i + 1; j < map.size(); ++j) {
            if (map[i].range.intersects(map[j].range)) {
                return false;
            }
        }
    }
    return true;
}

bool test_boot_map_is_ordered(const TestContext& ctx) noexcept {
    const auto map = ctx.memory.regions();
    for (size_t i = 1; i < map.size(); ++i) {
        const auto previous_end = map[i - 1].range.limit();
        if (!previous_end || *previous_end > map[i].range.base()) {
            return false;
        }
    }
    return true;
}

bool test_kernel_image_has_exact_region(const TestContext& ctx) noexcept {
    const auto boot_range = boot_layout.entry.pages();
    const auto secondary_range = boot_layout.secondary.pages();
    const auto image_range = boot_layout.kernel.pages();
    const auto boot_end = boot_range.limit();
    const auto secondary_end = secondary_range.limit();
    const auto image_end = image_range.limit();
    if (!boot_end || !secondary_end || !image_end) {
        return false;
    }
    bool boot_entry{};
    bool secondary{};
    bool high_image{};
    for (const auto& region : ctx.memory.regions()) {
        const auto end = region.range.limit();
        if (!end) {
            return false;
        }
        const uintptr_t first = region.range.base().base().raw();
        const uintptr_t last = end->raw() * mm::page_size;
        boot_entry |= region.kind == mm::Region::Kind::Boot
            && first == boot_range.base().base().raw()
            && last == boot_end->raw() * mm::page_size;
        secondary |= region.kind == mm::Region::Kind::Kernel
            && first == secondary_range.base().base().raw()
            && last == secondary_end->raw() * mm::page_size;
        high_image |= region.kind == mm::Region::Kind::Kernel
            && first == image_range.base().base().raw()
            && last == image_end->raw() * mm::page_size;
    }
    return boot_entry && secondary && high_image;
}

bool test_fdt_pages_are_reclaimable(const TestContext& ctx) noexcept {
    if (!ctx.boot.firmware) {
        return false;
    }
    for (const auto& region : ctx.memory.regions()) {
        if (region.kind == mm::Region::Kind::Boot
            && region.range.base() == ctx.boot.firmware.pages().base()
            && region.range.page_count()
                == ctx.boot.firmware.pages().page_count()) {
            return true;
        }
    }
    return false;
}

bool test_transition_pages_are_reclaimable(const TestContext& ctx) noexcept {
    if (!boot_layout.scratch.pages().valid()) {
        return false;
    }
    for (const auto& region : ctx.memory.regions()) {
        if (region.kind == mm::Region::Kind::Boot
            && region.range.base() == boot_layout.scratch.pages().base()
            && region.range.page_count()
                == boot_layout.scratch.pages().page_count()) {
            return true;
        }
    }
    return false;
}

bool test_region_policy_is_explicit(const TestContext&) noexcept {
    const mm::Region available{
        mm::Pages{mm::Page{1}, 1},
        mm::Region::Kind::Ram,
    };
    const mm::Region reclaimable{
        mm::Pages{mm::Page{2}, 1},
        mm::Region::Kind::Boot,
    };
    const mm::Region kernel{
        mm::Pages{mm::Page{3}, 1},
        mm::Region::Kind::Kernel,
    };
    const mm::Region mmio{
        mm::Pages{mm::Page{4}, 1},
        mm::Region::Kind::Mmio,
    };
    return available.is_ram() && !available.is_reclaimable()
        && reclaimable.is_ram() && reclaimable.is_reclaimable()
        && kernel.is_ram() && !kernel.is_reclaimable()
        && !mmio.is_ram();
}

using Kind = mm::Region::Kind;
static auto region(uintptr_t p, size_t n, Kind k = Kind::Ram) noexcept -> mm::Region {
    return {mm::Pages{mm::Page{p}, n}, k};
}

bool test_inventory_banks(const TestContext&) noexcept {
    mm::RegionList map{region(100, 8), region(200, 4), region(102, 2, Kind::Firmware),
                       region(103, 2, Kind::Kernel), region(201, 1, Kind::Boot)};
    if (!mm::normalize(map)) return false;
    const mm::Region expected[]{region(100, 2), region(102, 1, Kind::Firmware),
        region(103, 2, Kind::Kernel), region(105, 3), region(200, 1),
        region(201, 1, Kind::Boot), region(202, 2)};
    return map.size() == std::size(expected) && std::ranges::equal(map, expected,
        [](const auto& a, const auto& b) { return a.range == b.range && a.kind == b.kind; });
}

bool test_permanent_reservation_overrides_reclaimable(const TestContext&) noexcept {
    mm::RegionList map{region(300, 8), region(301, 5, Kind::Boot), region(303, 1, Kind::Firmware)};
    if (!mm::normalize(map) || map.size() != 5) return false;
    return map[1].kind == Kind::Boot && map[1].range.page_count() == 2
        && map[2].kind == Kind::Firmware && map[2].range.page_count() == 1
        && map[3].kind == Kind::Boot && map[3].range.page_count() == 2;
}

bool test_adjacent_reclaimable_resources_keep_boundaries(const TestContext&) noexcept {
    mm::RegionList map{region(500, 8), region(501, 2, Kind::Boot), region(503, 2, Kind::Boot)};
    if (!mm::normalize(map) || map.size() != 4) return false;
    return map[1].kind == Kind::Boot && map[1].range == mm::Pages{mm::Page{501}, 2}
        && map[2].kind == Kind::Boot && map[2].range == mm::Pages{mm::Page{503}, 2};
}

bool test_inventory_ram_overlap(const TestContext&) noexcept {
    mm::RegionList map{region(400, 8), region(404, 8)};
    const auto result = mm::normalize(map);
    return !result && result.error() == mm::PmmInitError::OverlappingRegions;
}

bool test_inventory_needs_ram(const TestContext&) noexcept {
    mm::RegionList map{};
    const auto result = mm::normalize(map);
    return !result && result.error() == mm::PmmInitError::NoRam;
}

bool test_inventory_rejects_conflicting_ownership(const TestContext&) noexcept {
    mm::RegionList conflict{region(100, 8), region(102, 1, Kind::Mmio)};
    const auto a = mm::normalize(conflict);
    mm::RegionList outside{region(100, 8), region(200, 1, Kind::Kernel)};
    const auto b = mm::normalize(outside);
    mm::RegionList valid{region(100, 8), region(200, 2, Kind::Mmio), region(300, 4, Kind::Firmware)};
    return !a && a.error() == mm::PmmInitError::OverlappingRegions
        && !b && b.error() == mm::PmmInitError::InvalidRegion
        && mm::normalize(valid) && valid.size() == 2
        && valid[0].range == mm::Pages{mm::Page{100}, 8}
        && valid[1].kind == Kind::Mmio;
}

bool test_byte_ranges_have_explicit_page_rounding(const TestContext&) noexcept {
    const auto contained = mm::Pages::contained_bytes(
        mm::pa_t{0x1003},
        0x2ffe);
    const auto covering = mm::Pages::covering_bytes(
        mm::pa_t{0x1003},
        0x2ffe);
    return contained
        && contained->base().raw() == 2
        && contained->page_count() == 2
        && covering
        && covering->base().raw() == 1
        && covering->page_count() == 4;
}

bool test_fdt_memory_reservations_are_bounded(const TestContext&) noexcept {
    alignas(8) uint8_t blob[88]{};
    auto be32 = [&blob](size_t offset, uint32_t value) {
        blob[offset] = static_cast<uint8_t>(value >> 24);
        blob[offset + 1] = static_cast<uint8_t>(value >> 16);
        blob[offset + 2] = static_cast<uint8_t>(value >> 8);
        blob[offset + 3] = static_cast<uint8_t>(value);
    };
    be32(0, 0xd00dfeed);
    be32(4, sizeof(blob));
    be32(8, 72);
    be32(12, 88);
    be32(16, 40);
    be32(20, 17);
    be32(24, 16);
    be32(36, 16);
    be32(44, 0x1000);
    be32(52, 0x2000);
    be32(72, 1);
    be32(80, 2);
    be32(84, 9);
    const auto tree = libk::fdt::open(std::span{blob});
    size_t visits = 0;
    const bool valid = tree && tree.value().for_each_reservation(
        [&visits](uint64_t address, uint64_t size) {
            ++visits;
            return address == 0x1000 && size == 0x2000;
        });
    blob[71] = 1; // The reservation terminator no longer fits in its block.
    const auto unterminated = libk::fdt::open(std::span{blob});
    return valid && visits == 1 && !unterminated
        && !libk::fdt::open(std::span{blob}.first(39));
}

} // namespace

void register_bootinfo_tests(TestRegistry& registry) noexcept {
    (void)registry.add("boot-map", "default firmware handoff is empty", test_default_bootinfo_is_cleared);
    (void)registry.add("boot-map", "regions are valid and non-overlapping", test_boot_map_is_valid_and_non_overlapping);
    (void)registry.add("boot-map", "regions are ordered by address", test_boot_map_is_ordered);
    (void)registry.add("boot-map", "kernel image owns only persistent load regions", test_kernel_image_has_exact_region);
    (void)registry.add("boot-map", "FDT pages are reclaimable", test_fdt_pages_are_reclaimable);
    (void)registry.add("boot-map", "transitional tables are reclaimable", test_transition_pages_are_reclaimable);
    (void)registry.add("boot-map", "region policy is explicit", test_region_policy_is_explicit);
    (void)registry.add("boot-map", "multiple RAM banks normalize into one inventory", test_inventory_banks);
    (void)registry.add("boot-map", "permanent reservations override reclaimable data", test_permanent_reservation_overrides_reclaimable);
    (void)registry.add("boot-map", "adjacent reclaimable resources keep lifetime boundaries", test_adjacent_reclaimable_resources_keep_boundaries);
    (void)registry.add("boot-map", "overlapping RAM banks are rejected", test_inventory_ram_overlap);
    (void)registry.add("boot-map", "an inventory requires RAM", test_inventory_needs_ram);
    (void)registry.add("boot-map", "physical inventory rejects conflicting owners", test_inventory_rejects_conflicting_ownership);
    (void)registry.add("boot-map", "byte ranges state their page rounding", test_byte_ranges_have_explicit_page_rounding);
    (void)registry.add("boot-map", "FDT memory reservations stay within their block", test_fdt_memory_reservations_are_bounded);
}
