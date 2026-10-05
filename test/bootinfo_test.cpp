#include <test/test.hpp>

#include <boot/fdt.hpp>
#include <utility>
#include <mm/phys.hpp>
#include <mm/pmm.hpp>
#include <boot/link.hpp>

namespace {

bool test_default_bootinfo_is_cleared(const TestContext&) noexcept {
    BootInfo boot{};
    return !boot.fdt
        && !boot.transition.valid()
        && boot.fdt.physical.raw() == 0
        && boot.fdt.size == 0
        && !boot.fdt.pages.valid();
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
    const auto boot_range = boot_pages();
    const auto secondary_range = secondary_pages();
    const auto image_range = kernel_pages();
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
        if (region.kind != mm::Region::Kind::Kernel) {
            continue;
        }
        const auto end = region.range.limit();
        if (!end) {
            return false;
        }
        const uintptr_t first = region.range.base().base().raw();
        const uintptr_t last = end->raw() * mm::page_size;
        boot_entry |= first == boot_range.base().base().raw()
            && last == boot_end->raw() * mm::page_size;
        secondary |= first == secondary_range.base().base().raw()
            && last == secondary_end->raw() * mm::page_size;
        high_image |= first == image_range.base().base().raw()
            && last == image_end->raw() * mm::page_size;
    }
    return boot_entry && secondary && high_image;
}

bool test_pre_kernel_ram_is_firmware_reserved(const TestContext& ctx) noexcept {
    const uintptr_t kernel_start =
        boot_pages().base().base().raw();
    for (const auto& region : ctx.memory.regions()) {
        if (region.kind != mm::Region::Kind::Firmware) {
            continue;
        }
        const auto end = region.range.limit();
        return end && end->raw() * mm::page_size == kernel_start;
    }
    return false;
}

bool test_fdt_pages_are_reclaimable(const TestContext& ctx) noexcept {
    if (!ctx.boot.fdt) {
        return false;
    }
    for (const auto& region : ctx.memory.regions()) {
        if (region.kind == mm::Region::Kind::Boot
            && region.range.base() == ctx.boot.fdt.pages.base()
            && region.range.page_count()
                == ctx.boot.fdt.pages.page_count()) {
            return true;
        }
    }
    return false;
}

bool test_transition_pages_are_reclaimable(const TestContext& ctx) noexcept {
    if (!ctx.boot.transition.valid()) {
        return false;
    }
    for (const auto& region : ctx.memory.regions()) {
        if (region.kind == mm::Region::Kind::Boot
            && region.range.base() == ctx.boot.transition.base()
            && region.range.page_count()
                == ctx.boot.transition.page_count()) {
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

bool test_builder_normalizes_multiple_ram_banks(const TestContext&) noexcept {
    using Kind = mm::Region::Kind;
    mm::PhysMap builder{};
    if (!builder.add_ram(mm::Pages{
            mm::Page{100}, 8})
        || !builder.add_ram(mm::Pages{
            mm::Page{200}, 4})
        || !builder.reserve(
            mm::Pages{mm::Page{102}, 2},
            Kind::Firmware)
        || !builder.reserve(
            mm::Pages{mm::Page{103}, 2},
            Kind::Kernel)
        || !builder.reserve(
            mm::Pages{mm::Page{201}, 1},
            Kind::Boot)) {
        return false;
    }

    mm::RegionList map{};
    const auto result = std::move(builder).finish(map);
    if (!result) {
        return false;
    }

    struct ExpectedRegion {
        uintptr_t first;
        size_t pages;
        Kind kind;
    };
    constexpr ExpectedRegion expected[] = {
        {100, 2, Kind::Ram},
        {102, 1, Kind::Firmware},
        {103, 2, Kind::Kernel},
        {105, 3, Kind::Ram},
        {200, 1, Kind::Ram},
        {201, 1, Kind::Boot},
        {202, 2, Kind::Ram},
    };
    if (map.size() != sizeof(expected) / sizeof(expected[0])) {
        return false;
    }
    for (size_t index = 0; index < map.size(); ++index) {
        if (map[index].range.base().raw() != expected[index].first
            || map[index].range.page_count() != expected[index].pages
            || map[index].kind != expected[index].kind) {
            return false;
        }
    }
    return true;
}

bool test_permanent_reservation_overrides_reclaimable(const TestContext&) noexcept {
    using Kind = mm::Region::Kind;
    mm::PhysMap builder{};
    if (!builder.add_ram(mm::Pages{
            mm::Page{300}, 8})
        || !builder.reserve(
            mm::Pages{mm::Page{301}, 5},
            Kind::Boot)
        || !builder.reserve(
            mm::Pages{mm::Page{303}, 1},
            Kind::Firmware)) {
        return false;
    }

    mm::RegionList map{};
    const auto result = std::move(builder).finish(map);
    if (!result) {
        return false;
    }
    if (map.size() != 5) {
        return false;
    }
    return map[1].kind == Kind::Boot
        && map[1].range.page_count() == 2
        && map[2].kind == Kind::Firmware
        && map[2].range.page_count() == 1
        && map[3].kind == Kind::Boot
        && map[3].range.page_count() == 2;
}

bool test_adjacent_reclaimable_resources_keep_boundaries(const TestContext&) noexcept {
    using Kind = mm::Region::Kind;
    mm::PhysMap builder{};
    if (!builder.add_ram(mm::Pages{
            mm::Page{500}, 8})
        || !builder.reserve(
            mm::Pages{mm::Page{501}, 2},
            Kind::Boot)
        || !builder.reserve(
            mm::Pages{mm::Page{503}, 2},
            Kind::Boot)) {
        return false;
    }

    mm::RegionList map{};
    if (!std::move(builder).finish(map) || map.size() != 4) {
        return false;
    }
    return map[1].kind == Kind::Boot
        && map[1].range.base().raw() == 501
        && map[1].range.page_count() == 2
        && map[2].kind == Kind::Boot
        && map[2].range.base().raw() == 503
        && map[2].range.page_count() == 2;
}

bool test_builder_rejects_overlapping_ram_banks(const TestContext&) noexcept {
    mm::PhysMap builder{};
    if (!builder.add_ram(mm::Pages{
            mm::Page{400}, 8})
        || !builder.add_ram(mm::Pages{
            mm::Page{404}, 8})) {
        return false;
    }
    mm::RegionList map{};
    const auto result = std::move(builder).finish(map);
    return !result && result.error() == mm::PhysErr::Overlap;
}

bool test_builder_requires_ram(const TestContext&) noexcept {
    mm::PhysMap builder{};
    mm::RegionList map{};
    const auto result = std::move(builder).finish(map);
    return !result && result.error() == mm::PhysErr::NoRam;
}

bool test_byte_ranges_have_explicit_page_rounding(const TestContext&) noexcept {
    const auto contained = mm::Pages::contained_bytes(
        mm::Phys{0x1003},
        0x2ffe);
    const auto covering = mm::Pages::covering_bytes(
        mm::Phys{0x1003},
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
    const auto tree = Fdt::open(blob);
    size_t visits = 0;
    const bool valid = tree && tree.value().for_each_reservation(
        [&visits](uint64_t address, uint64_t size) {
            ++visits;
            return address == 0x1000 && size == 0x2000;
        });
    blob[71] = 1; // The reservation terminator no longer fits in its block.
    const auto unterminated = Fdt::open(blob);
    return valid && visits == 1 && !unterminated;
}

} // namespace

void register_bootinfo_tests(TestRegistry& registry) noexcept {
    (void)registry.add("boot-map", "default firmware handoff is empty", test_default_bootinfo_is_cleared);
    (void)registry.add("boot-map", "regions are valid and non-overlapping", test_boot_map_is_valid_and_non_overlapping);
    (void)registry.add("boot-map", "regions are ordered by address", test_boot_map_is_ordered);
    (void)registry.add("boot-map", "kernel image owns only persistent load regions", test_kernel_image_has_exact_region);
    (void)registry.add("boot-map", "pre-kernel RAM stays firmware-reserved", test_pre_kernel_ram_is_firmware_reserved);
    (void)registry.add("boot-map", "FDT pages are reclaimable", test_fdt_pages_are_reclaimable);
    (void)registry.add("boot-map", "transitional tables are reclaimable", test_transition_pages_are_reclaimable);
    (void)registry.add("boot-map", "region policy is explicit", test_region_policy_is_explicit);
    (void)registry.add("boot-map", "multiple RAM banks normalize into one inventory", test_builder_normalizes_multiple_ram_banks);
    (void)registry.add("boot-map", "permanent reservations override reclaimable data", test_permanent_reservation_overrides_reclaimable);
    (void)registry.add("boot-map", "adjacent reclaimable resources keep lifetime boundaries", test_adjacent_reclaimable_resources_keep_boundaries);
    (void)registry.add("boot-map", "overlapping RAM banks are rejected", test_builder_rejects_overlapping_ram_banks);
    (void)registry.add("boot-map", "an inventory requires RAM", test_builder_requires_ram);
    (void)registry.add("boot-map", "byte ranges state their page rounding", test_byte_ranges_have_explicit_page_rounding);
    (void)registry.add("boot-map", "FDT memory reservations stay within their block", test_fdt_memory_reservations_are_bounded);
}
