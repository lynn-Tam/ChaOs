#include "range_map.hpp"

#include <libk/expected.hpp>

namespace arch::riscv64 {

auto map_range(
    Sv39Builder& builder,
    kernel::mm::VPage first_vpage,
    kernel::mm::PageRange physical,
    PtePerm permissions) noexcept -> Sv39Builder::MapResult {
    if (!physical.valid()) {
        return libk::unexpected(MappingError::BadPAddr);
    }

    for (size_t index = 0; index < physical.page_count(); ++index) {
        const auto physical_frame =
            physical.first().frame().checked_add(index);
        if (!physical_frame.has_value()) {
            return libk::unexpected(MappingError::BadPAddr);
        }

        const auto vpage = first_vpage.checked_add(index);
        if (!vpage.has_value()) {
            return libk::unexpected(MappingError::BadVAddr);
        }

        auto mapped = builder.map_page(
            vpage.value(),
            kernel::mm::Page{physical_frame.value()},
            permissions);
        if (!mapped.has_value()) {
            return mapped;
        }
    }

    return libk::expected();
}

auto maps_page(
    const Sv39Builder& builder,
    kernel::mm::VPage vpage,
    kernel::mm::Page physical,
    PtePerm permissions) noexcept -> bool {
    const auto entry = builder.mapping_at(vpage);
    if (!entry.has_value()) {
        return false;
    }

    const Pte& leaf = entry.value();
    const auto mapped_page = leaf.leaf_page();
    return mapped_page.has_value()
        && mapped_page.value() == physical
        && leaf.has_permissions(permissions);
}

auto maps_range(
    const Sv39Builder& builder,
    kernel::mm::VPage first_vpage,
    kernel::mm::PageRange physical,
    PtePerm permissions) noexcept -> bool {
    if (!physical.valid()) {
        return false;
    }

    for (size_t index = 0; index < physical.page_count(); ++index) {
        const auto physical_frame =
            physical.first().frame().checked_add(index);
        const auto vpage = first_vpage.checked_add(index);
        if (!physical_frame.has_value() || !vpage.has_value()) {
            return false;
        }

        if (!maps_page(
                builder,
                vpage.value(),
                kernel::mm::Page{physical_frame.value()},
                permissions)) {
            return false;
        }
    }

    return true;
}

} // namespace arch::riscv64
