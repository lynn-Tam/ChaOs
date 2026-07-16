#pragma once

#include "arch/riscv64/sbi/call.hpp"

namespace arch::riscv64::sbi {

inline constexpr usize ipi_extension_id = 0x735049;

struct HartMask final {
    usize bits{};
    usize base{};
};

[[nodiscard]] constexpr auto single_hart_mask(usize hart_id) noexcept
    -> HartMask {
    constexpr usize width = sizeof(usize) * 8;
    const usize base = hart_id & ~(width - 1);
    return HartMask{usize{1} << (hart_id - base), base};
}

static_assert(single_hart_mask(0).bits == 1
    && single_hart_mask(0).base == 0);
static_assert(single_hart_mask(63).bits == (usize{1} << 63)
    && single_hart_mask(63).base == 0);
static_assert(single_hart_mask(64).bits == 1
    && single_hart_mask(64).base == 64);
static_assert(single_hart_mask(130).bits == 4
    && single_hart_mask(130).base == 128);

[[nodiscard]] inline auto send_ipi(
    usize hart_mask,
    usize hart_mask_base) noexcept -> Ret {
    constexpr usize send_ipi_function = 0;
    return call(
        ipi_extension_id, send_ipi_function, hart_mask, hart_mask_base);
}

} // namespace arch::riscv64::sbi
