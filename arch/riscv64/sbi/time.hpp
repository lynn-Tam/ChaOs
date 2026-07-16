#pragma once

#include "arch/riscv64/sbi/call.hpp"

namespace arch::riscv64::sbi {

inline constexpr usize time_extension_id = 0x54494d45;

[[nodiscard]] inline auto set_timer(u64 absolute_time) noexcept -> Ret {
    constexpr usize set_timer_function = 0;
    return call(
        time_extension_id,
        set_timer_function,
        static_cast<usize>(absolute_time));
}

} // namespace arch::riscv64::sbi
