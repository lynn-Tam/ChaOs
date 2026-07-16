#pragma once

#include <arch/backend/address_layout.hpp>

#include <core/types.hpp>
#include <mm/addr.hpp>

namespace arch::layout {

inline constexpr usize low_guard_end = backend::layout::LowGuardEnd;
inline constexpr usize user_end = backend::layout::UserEnd;
inline constexpr usize direct_base = backend::layout::DirectBase;
inline constexpr usize direct_end = backend::layout::DirectEnd;
inline constexpr usize dynamic_base = backend::layout::DynamicBase;
inline constexpr usize kernel_base = backend::layout::KernelBase;
inline constexpr usize direct_size = direct_end - direct_base;

[[nodiscard]] constexpr auto is_user(kernel::mm::VirtAddr address) noexcept -> bool {
    return address.raw() >= low_guard_end && address.raw() < user_end;
}

[[nodiscard]] constexpr auto is_kernel(kernel::mm::VirtAddr address) noexcept -> bool {
    return address.raw() >= direct_base;
}

static_assert(low_guard_end < user_end);
static_assert(user_end <= direct_base);
static_assert(direct_base < direct_end);
static_assert(direct_end <= kernel_base);
static_assert(direct_size == 128ULL * 1024 * 1024 * 1024);
static_assert((direct_base & (kernel::mm::page_size - 1)) == 0);
static_assert((kernel_base & ((2ULL * 1024 * 1024) - 1)) == 0);

} // namespace arch::layout
