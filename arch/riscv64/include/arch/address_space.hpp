#pragma once

#include <core/types.hpp>
#include <mm/addr.hpp>

namespace arch::address_space {

// Sv39 sign-extends bit 38. These bounds describe what the translation mode
// can represent; the kernel's use of each canonical half belongs to MM policy.
inline constexpr usize LowerCanonicalEnd = 0x0000004000000000ULL;
inline constexpr usize UpperCanonicalBegin = 0xffffffc000000000ULL;

[[nodiscard]] constexpr auto is_lower(
    kernel::mm::VirtAddr address) noexcept -> bool {
    return address.raw() < LowerCanonicalEnd;
}

[[nodiscard]] constexpr auto is_upper(
    kernel::mm::VirtAddr address) noexcept -> bool {
    return address.raw() >= UpperCanonicalBegin;
}

[[nodiscard]] constexpr auto is_canonical(
    kernel::mm::VirtAddr address) noexcept -> bool {
    return is_lower(address) || is_upper(address);
}

static_assert(LowerCanonicalEnd == usize{1} << 38);
static_assert(UpperCanonicalBegin
    == ~static_cast<usize>(LowerCanonicalEnd - 1));

} // namespace arch::address_space
