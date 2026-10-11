#pragma once

#include <libk/types.hpp>
#include <source_location>
#include <libk/assert.hpp>
#include <atomic>
#include <cpu/types.hpp>
#include <cpu.hpp>
#include <trap.hpp>

class Cpus;

struct PanicSlot final {
    std::atomic<bool> stopped{};
    CpuId cpu{};
    CpuHwId hardware{};
    const char* reason{};
    libk::assert_info site{};
    arch::StackRegs stack{};
    arch::TrapRegs trap{};
    Cpus* registry{};
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
