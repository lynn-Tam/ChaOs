#pragma once

#include <arch/address_space.hpp>
#include <core/types.hpp>
#include <mm/addr.hpp>

namespace kernel::mm::layout {

inline constexpr usize LowGuardEnd = 64 * 1024;
inline constexpr usize UserEnd = arch::address_space::LowerCanonicalEnd;

inline constexpr usize DirectMapBegin = arch::address_space::UpperCanonicalBegin;
inline constexpr usize DirectMapSize = 128ULL * 1024 * 1024 * 1024;
inline constexpr usize DirectMapEnd = DirectMapBegin + DirectMapSize;
inline constexpr usize DynamicBegin = DirectMapEnd;

[[nodiscard]] constexpr auto is_user(VirtAddr address) noexcept -> bool {
    return address.raw() >= LowGuardEnd && address.raw() < UserEnd;
}

[[nodiscard]] constexpr auto is_kernel(VirtAddr address) noexcept -> bool {
    return arch::address_space::is_upper(address);
}

static_assert(LowGuardEnd < UserEnd);
static_assert(DirectMapEnd > DirectMapBegin);
static_assert(arch::address_space::is_upper(VirtAddr{DirectMapBegin}));
static_assert(arch::address_space::is_upper(VirtAddr{DirectMapEnd}));

} // namespace kernel::mm::layout
