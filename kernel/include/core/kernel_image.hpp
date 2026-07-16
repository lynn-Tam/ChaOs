#pragma once

#include <libk/optional.hpp>
#include <mm/addr.hpp>

namespace kernel::image {

[[nodiscard]] auto virtual_begin() noexcept -> mm::VirtAddr;
[[nodiscard]] auto virtual_end() noexcept -> mm::VirtAddr;
[[nodiscard]] auto physical_begin() noexcept -> mm::PhysAddr;

[[nodiscard]] auto boot_entry() noexcept -> mm::PageRange;
[[nodiscard]] auto secondary_entry() noexcept -> mm::PageRange;
[[nodiscard]] auto transition() noexcept -> mm::PageRange;
[[nodiscard]] auto physical_image() noexcept -> mm::PageRange;

// Converts only addresses inside the statically linked high kernel image.
// Runtime RAM translation remains owned by mm::DirectMap.
[[nodiscard]] auto linked_physical(mm::VirtAddr address) noexcept
    -> libk::optional<mm::PhysAddr>;

} // namespace kernel::image
