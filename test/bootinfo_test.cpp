#include <test/test.hpp>

#include <boot/firmware/devicetree/fdt.hpp>
#include <libk/utility.hpp>
#include <mm/boot_map.hpp>
#include <mm/pmm.hpp>
#include <core/kernel_image.hpp>

namespace {

bool test_default_bootinfo_is_cleared(const TestContext&) noexcept {
    kernel::boot::BootInfo boot{};
    return !boot.fdt
        && !boot.transition
        && boot.fdt.physical.raw() == 0
        && boot.fdt.size == 0
        && !boot.fdt.pages.valid()
        && boot.memory_regions.empty();
}

bool test_boot_map_is_valid_and_non_overlapping(const TestContext& ctx) noexcept {
    const auto map = ctx.boot.memory_regions.span();
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
    const auto map = ctx.boot.memory_regions.span();
    for (size_t i = 1; i < map.size(); ++i) {
        const auto previous_end = map[i - 1].range.end_frame();
        if (!previous_end || *previous_end > map[i].range.first().frame()) {
            return false;
        }
    }
    return true;
}

bool test_kernel_image_has_exact_region(const TestContext& ctx) noexcept {
    const auto boot_range = kernel::image::boot_entry();
    const auto secondary_range = kernel::image::secondary_entry();
    const auto image_range = kernel::image::physical_image();
    const auto boot_end = boot_range.end_frame();
    const auto secondary_end = secondary_range.end_frame();
    const auto image_end = image_range.end_frame();
    if (!boot_end || !secondary_end || !image_end) {
        return false;
    }
    bool boot_entry{};
    bool secondary{};
    bool high_image{};
    for (const auto& region : ctx.boot.memory_regions) {
        if (region.kind != kernel::mm::RegionKind::KernelImage) {
            continue;
        }
        const auto end = region.range.end_frame();
        if (!end) {
            return false;
        }
        const uintptr_t first = region.range.first().base().raw();
        const uintptr_t last = end->raw() * kernel::mm::page_size;
        boot_entry |= first == boot_range.first().base().raw()
            && last == boot_end->raw() * kernel::mm::page_size;
        secondary |= first == secondary_range.first().base().raw()
            && last == secondary_end->raw() * kernel::mm::page_size;
        high_image |= first == image_range.first().base().raw()
            && last == image_end->raw() * kernel::mm::page_size;
    }
    return boot_entry && secondary && high_image;
}

bool test_pre_kernel_ram_is_firmware_reserved(const TestContext& ctx) noexcept {
    const uintptr_t kernel_start =
        kernel::image::boot_entry().first().base().raw();
    for (const auto& region : ctx.boot.memory_regions) {
        if (region.kind != kernel::mm::RegionKind::FirmwareReserved) {
            continue;
        }
        const auto end = region.range.end_frame();
        return end && end->raw() * kernel::mm::page_size == kernel_start;
    }
    return false;
}

bool test_fdt_pages_are_reclaimable(const TestContext& ctx) noexcept {
    if (!ctx.boot.fdt) {
        return false;
    }
    for (const auto& region : ctx.boot.memory_regions) {
        if (region.kind == kernel::mm::RegionKind::ReclaimableBootData
            && region.range.first() == ctx.boot.fdt.pages.first()
            && region.range.page_count()
                == ctx.boot.fdt.pages.page_count()) {
            return true;
        }
    }
    return false;
}

bool test_transition_pages_are_reclaimable(const TestContext& ctx) noexcept {
    if (!ctx.boot.transition) {
        return false;
    }
    for (const auto& region : ctx.boot.memory_regions) {
        if (region.kind == kernel::mm::RegionKind::ReclaimableBootData
            && region.range.first() == ctx.boot.transition.pages.first()
            && region.range.page_count()
                == ctx.boot.transition.pages.page_count()) {
            return true;
        }
    }
    return false;
}

bool test_region_policy_is_explicit(const TestContext&) noexcept {
    const kernel::mm::Region available{
        kernel::mm::PageRange{kernel::mm::Page{kernel::mm::Pfn{1}}, 1},
        kernel::mm::RegionKind::AvailableRam,
    };
    const kernel::mm::Region reclaimable{
        kernel::mm::PageRange{kernel::mm::Page{kernel::mm::Pfn{2}}, 1},
        kernel::mm::RegionKind::ReclaimableBootData,
    };
    const kernel::mm::Region kernel{
        kernel::mm::PageRange{kernel::mm::Page{kernel::mm::Pfn{3}}, 1},
        kernel::mm::RegionKind::KernelImage,
    };
    const kernel::mm::Region mmio{
        kernel::mm::PageRange{kernel::mm::Page{kernel::mm::Pfn{4}}, 1},
        kernel::mm::RegionKind::Mmio,
    };
    return available.is_ram() && !available.is_reclaimable()
        && reclaimable.is_ram() && reclaimable.is_reclaimable()
        && kernel.is_ram() && !kernel.is_reclaimable()
        && !mmio.is_ram();
}

bool test_builder_normalizes_multiple_ram_banks(const TestContext&) noexcept {
    using Kind = kernel::mm::RegionKind;
    kernel::mm::BootMapBuilder builder{};
    if (!builder.add_ram(kernel::mm::PageRange{
            kernel::mm::Page{kernel::mm::Pfn{100}}, 8})
        || !builder.add_ram(kernel::mm::PageRange{
            kernel::mm::Page{kernel::mm::Pfn{200}}, 4})
        || !builder.reserve(
            kernel::mm::PageRange{kernel::mm::Page{kernel::mm::Pfn{102}}, 2},
            Kind::FirmwareReserved)
        || !builder.reserve(
            kernel::mm::PageRange{kernel::mm::Page{kernel::mm::Pfn{103}}, 2},
            Kind::KernelImage)
        || !builder.reserve(
            kernel::mm::PageRange{kernel::mm::Page{kernel::mm::Pfn{201}}, 1},
            Kind::ReclaimableBootData)) {
        return false;
    }

    kernel::mm::RegionList map{};
    const auto result = libk::move(builder).build_into(map);
    if (!result) {
        return false;
    }

    struct ExpectedRegion {
        uintptr_t first;
        size_t pages;
        Kind kind;
    };
    constexpr ExpectedRegion expected[] = {
        {100, 2, Kind::AvailableRam},
        {102, 1, Kind::FirmwareReserved},
        {103, 2, Kind::KernelImage},
        {105, 3, Kind::AvailableRam},
        {200, 1, Kind::AvailableRam},
        {201, 1, Kind::ReclaimableBootData},
        {202, 2, Kind::AvailableRam},
    };
    if (map.size() != sizeof(expected) / sizeof(expected[0])) {
        return false;
    }
    for (size_t index = 0; index < map.size(); ++index) {
        if (map[index].range.first().frame().raw() != expected[index].first
            || map[index].range.page_count() != expected[index].pages
            || map[index].kind != expected[index].kind) {
            return false;
        }
    }
    return true;
}

bool test_permanent_reservation_overrides_reclaimable(const TestContext&) noexcept {
    using Kind = kernel::mm::RegionKind;
    kernel::mm::BootMapBuilder builder{};
    if (!builder.add_ram(kernel::mm::PageRange{
            kernel::mm::Page{kernel::mm::Pfn{300}}, 8})
        || !builder.reserve(
            kernel::mm::PageRange{kernel::mm::Page{kernel::mm::Pfn{301}}, 5},
            Kind::ReclaimableBootData)
        || !builder.reserve(
            kernel::mm::PageRange{kernel::mm::Page{kernel::mm::Pfn{303}}, 1},
            Kind::FirmwareReserved)) {
        return false;
    }

    kernel::mm::RegionList map{};
    const auto result = libk::move(builder).build_into(map);
    if (!result) {
        return false;
    }
    if (map.size() != 5) {
        return false;
    }
    return map[1].kind == Kind::ReclaimableBootData
        && map[1].range.page_count() == 2
        && map[2].kind == Kind::FirmwareReserved
        && map[2].range.page_count() == 1
        && map[3].kind == Kind::ReclaimableBootData
        && map[3].range.page_count() == 2;
}

bool test_adjacent_reclaimable_resources_keep_boundaries(const TestContext&) noexcept {
    using Kind = kernel::mm::RegionKind;
    kernel::mm::BootMapBuilder builder{};
    if (!builder.add_ram(kernel::mm::PageRange{
            kernel::mm::Page{kernel::mm::Pfn{500}}, 8})
        || !builder.reserve(
            kernel::mm::PageRange{kernel::mm::Page{kernel::mm::Pfn{501}}, 2},
            Kind::ReclaimableBootData)
        || !builder.reserve(
            kernel::mm::PageRange{kernel::mm::Page{kernel::mm::Pfn{503}}, 2},
            Kind::ReclaimableBootData)) {
        return false;
    }

    kernel::mm::RegionList map{};
    if (!libk::move(builder).build_into(map) || map.size() != 4) {
        return false;
    }
    return map[1].kind == Kind::ReclaimableBootData
        && map[1].range.first().frame().raw() == 501
        && map[1].range.page_count() == 2
        && map[2].kind == Kind::ReclaimableBootData
        && map[2].range.first().frame().raw() == 503
        && map[2].range.page_count() == 2;
}

bool test_builder_rejects_overlapping_ram_banks(const TestContext&) noexcept {
    kernel::mm::BootMapBuilder builder{};
    if (!builder.add_ram(kernel::mm::PageRange{
            kernel::mm::Page{kernel::mm::Pfn{400}}, 8})
        || !builder.add_ram(kernel::mm::PageRange{
            kernel::mm::Page{kernel::mm::Pfn{404}}, 8})) {
        return false;
    }
    kernel::mm::RegionList map{};
    const auto result = libk::move(builder).build_into(map);
    return !result && result.error() == kernel::mm::BootMapError::OverlappingRam;
}

bool test_builder_requires_ram(const TestContext&) noexcept {
    kernel::mm::BootMapBuilder builder{};
    kernel::mm::RegionList map{};
    const auto result = libk::move(builder).build_into(map);
    return !result && result.error() == kernel::mm::BootMapError::NoRam;
}

bool test_byte_ranges_have_explicit_page_rounding(const TestContext&) noexcept {
    const auto contained = kernel::mm::PageRange::contained_bytes(
        kernel::mm::PhysAddr{0x1003},
        0x2ffe);
    const auto covering = kernel::mm::PageRange::covering_bytes(
        kernel::mm::PhysAddr{0x1003},
        0x2ffe);
    return contained
        && contained->first().frame().raw() == 2
        && contained->page_count() == 2
        && covering
        && covering->first().frame().raw() == 1
        && covering->page_count() == 4;
}

bool test_fdt_memory_reservations_are_bounded(const TestContext&) noexcept {
    constexpr uint8_t reservations[] = {
        0, 0, 0, 0, 0, 0, 0x10, 0,
        0, 0, 0, 0, 0, 0, 0x20, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
    };
    kernel::boot::fdt::FDT_View view{};
    view.mem_rsvmap = reservations;
    view.mem_rsvmap_size = sizeof(reservations);
    size_t visits = 0;
    const bool valid = kernel::boot::fdt::visit_memory_reservations(
        view,
        [&visits](uint64_t address, uint64_t size) {
            ++visits;
            return address == 0x1000 && size == 0x2000;
        });
    view.mem_rsvmap_size -= 1;
    const bool truncated = kernel::boot::fdt::visit_memory_reservations(
        view,
        [](uint64_t, uint64_t) { return true; });
    return valid && visits == 1 && !truncated;
}

} // namespace

void register_bootinfo_tests(TestRegistry& registry) noexcept {
    (void)registry.add("boot-map", "default BootInfo owns an empty map", test_default_bootinfo_is_cleared);
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
