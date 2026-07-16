// arch/riscv64/trap/context.hpp
// RISC-V raw TrapFrame 到公共 TrapContext 的私有适配入口。

#pragma once

#include "arch/riscv64/trap/trapframe.hpp"

#include <arch/trap.hpp>

namespace arch::riscv64 {

[[nodiscard]] auto make_context(TrapFrame& frame) noexcept -> arch::TrapContext;

} // namespace arch::riscv64
