#pragma once

#include "arch/riscv64/sbi/call.hpp"

namespace arch::riscv64::sbi {

inline constexpr usize base_extension_id = 0x10;

[[nodiscard]] inline auto extension_available(usize extension) noexcept
    -> bool {
    constexpr usize probe_extension = 3;
    const Ret result = call(base_extension_id, probe_extension, extension);
    return result.error == success && result.value != 0;
}

} // namespace arch::riscv64::sbi
