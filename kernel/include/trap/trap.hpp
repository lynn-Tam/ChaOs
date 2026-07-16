// kernel/include/trap/trap.hpp
// 内核 trap policy 入口。架构层提供 Event 和可修改的返回现场视图。

#pragma once

#include <arch/trap.hpp>
#include <trap/event.hpp>

namespace kernel::trap {

void handle(const Event& event, arch::TrapContext& context) noexcept;

// Called after logical trap completion and trap_depth decrement while the
// original TrapFrame remains on the interrupted execution's stack. The
// dispatcher attaches its preemptive commit boundary here.
void on_exit(arch::TrapContext& context) noexcept;

} // namespace kernel::trap
