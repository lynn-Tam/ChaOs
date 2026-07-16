#pragma once

#include <core/types.hpp>

namespace arch::riscv64::sbi {

struct Ret final {
    isize error;
    usize value;
};

inline constexpr isize success = 0;
inline constexpr isize failed = -1;
inline constexpr isize not_supported = -2;
inline constexpr isize invalid_parameter = -3;
inline constexpr isize denied = -4;
inline constexpr isize invalid_address = -5;
inline constexpr isize already_available = -6;
inline constexpr isize already_started = -7;
inline constexpr isize already_stopped = -8;

[[nodiscard]] auto call(
    usize extension,
    usize function,
    usize arg0 = 0,
    usize arg1 = 0,
    usize arg2 = 0) noexcept -> Ret;

} // namespace arch::riscv64::sbi
