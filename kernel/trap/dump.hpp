// kernel/trap/dump.hpp
// Private trap diagnostics used by kernel trap policy fail-stop paths.

#pragma once

#include <arch/trap.hpp>
#include <trap/event.hpp>

namespace kernel::trap {

[[noreturn]] void panic_unhandled(
    const Event& event,
    const arch::TrapContext& context) noexcept;

} // namespace kernel::trap
