#pragma once

#include <core/types.hpp>

extern "C" char boot_stack_top[];
extern "C" [[nodiscard]] auto arch_boot_stack_guard_intact() noexcept -> bool;

namespace arch {

[[nodiscard]] inline auto boot_stack_top() noexcept -> usize {
    return reinterpret_cast<usize>(::boot_stack_top);
}

} // namespace arch
