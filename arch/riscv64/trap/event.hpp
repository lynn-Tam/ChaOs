// arch/riscv64/trap/event.hpp
// RISC-V 私有解码入口：把保存的 trap frame 转成内核可消费的 Event。

#pragma once

#include "arch/riscv64/trap/trapframe.hpp"

#include <trap/event.hpp>

namespace arch::riscv64 {

[[nodiscard]] auto make_event(const TrapFrame& frame) noexcept -> kernel::trap::Event;

} // namespace arch::riscv64
