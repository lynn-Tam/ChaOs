#pragma once

#include <platform/backend/memory_layout.hpp>

#include <arch/address_layout.hpp>
#include <libk/optional.hpp>
#include <mm/addr.hpp>

namespace platform::memory {

inline constexpr usize boot_physical_base = backend::memory::BootPhysicalBase;
inline constexpr usize boot_entry_size = backend::memory::BootEntrySize;
inline constexpr usize secondary_entry_physical =
    backend::memory::SecondaryEntryPhysical;
inline constexpr usize secondary_entry_size = backend::memory::SecondaryEntrySize;
inline constexpr usize transition_physical_base =
    backend::memory::TransitionPhysicalBase;
inline constexpr usize transition_size = backend::memory::TransitionSize;
inline constexpr usize kernel_physical_base = backend::memory::KernelPhysicalBase;

// This is the selected platform's static kernel-image load relation. General
// RAM translation remains owned by kernel::mm::DirectMap.
[[nodiscard]] constexpr auto linked_physical(kernel::mm::VirtAddr address) noexcept
    -> libk::optional<kernel::mm::PhysAddr> {
    if (address.raw() < arch::layout::kernel_base) {
        return libk::nullopt;
    }
    return kernel::mm::PhysAddr{kernel_physical_base}.checked_add(
        address.raw() - arch::layout::kernel_base);
}

} // namespace platform::memory
