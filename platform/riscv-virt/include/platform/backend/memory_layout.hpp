#pragma once

#include <core/types.hpp>
#include <mm/addr.hpp>

namespace platform::backend::memory {

inline constexpr usize BootPhysicalBase = 0x80200000ULL;
inline constexpr usize BootEntrySize = kernel::mm::page_size;
inline constexpr usize SecondaryEntryPhysical = BootPhysicalBase + 0x10000ULL;
inline constexpr usize SecondaryEntrySize = kernel::mm::page_size;
inline constexpr usize TransitionPhysicalBase =
    SecondaryEntryPhysical + SecondaryEntrySize;
inline constexpr usize TransitionSize = 4 * kernel::mm::page_size;
inline constexpr usize KernelPhysicalBase = 0x80400000ULL;

} // namespace platform::backend::memory
