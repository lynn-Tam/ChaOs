#pragma once

#include "arch/riscv64/sbi/call.hpp"

namespace arch::riscv64::sbi {

inline constexpr usize hsm_extension_id = 0x48534d;

[[nodiscard]] inline auto hart_start(
    usize hart_id,
    usize start_address,
    usize opaque) noexcept -> Ret {
    constexpr usize hart_start_function = 0;
    return call(
        hsm_extension_id,
        hart_start_function,
        hart_id,
        start_address,
        opaque);
}

} // namespace arch::riscv64::sbi
