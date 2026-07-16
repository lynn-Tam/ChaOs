#pragma once

#include <core/types.hpp>

namespace arch::backend::layout {

inline constexpr usize LowGuardEnd = 0x0000000000010000ULL;
inline constexpr usize UserEnd = 0x0000004000000000ULL;
inline constexpr usize DirectBase = 0xffffffc000000000ULL;
inline constexpr usize DirectEnd = 0xffffffe000000000ULL;
inline constexpr usize DynamicBase = DirectEnd;
inline constexpr usize KernelBase = 0xffffffff80000000ULL;

} // namespace arch::backend::layout
