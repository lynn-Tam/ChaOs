#pragma once

#include <arch/trap.hpp>

namespace kernel::syscall {

enum class Disposition : u8 {
    Return,
    Yield,
    Block,
    Exit,
};

[[nodiscard]] auto handle(arch::TrapContext& context) noexcept
    -> Disposition;

} // namespace kernel::syscall
