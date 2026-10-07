#pragma once

#include <base/types.hpp>
#include <source_location>
#include <libk/assert.hpp>
#include <libk/sync/atomic.hpp>
#include <cpu/types.hpp>
#include <cpu.hpp>
#include <trap.hpp>

class CpuRegistry;

struct PanicSlot final {
    libk::Atomic<bool> stopped{};
    CpuId cpu{};
    CpuHwId hardware{};
    const char* reason{};
    libk::AssertInfo site{};
    arch::StackRegs stack{};
    arch::TrapRegs trap{};
    CpuRegistry* registry{};
    usize current_thread{};
    usize active_root{};
    usize trap_depth{};
    usize stack_base{};
    usize stack_top{};
    bool has_full_trap{};
    bool interrupts_enabled{};
};

[[noreturn]] void panic(
    const char* reason, const arch::TrapCtx* trap = nullptr,
    std::source_location site = std::source_location::current()) noexcept;
[[noreturn]] void panic_stop(const arch::TrapCtx&) noexcept;
