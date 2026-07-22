#pragma once

#include "arch/riscv64/cpu/local_entry.hpp"
#include "arch/riscv64/cpu/start_context.hpp"

#include <core/types.hpp>
#include <cpu/topology.hpp>
#include <libk/expected.hpp>

namespace kernel {
class KernelState;
struct CpuRuntime;
}

namespace kernel::mm {
class DirectMap;
}

namespace arch {

using CpuEntryState = riscv64::CpuEntryBlock;
using CpuStartContext = riscv64::CpuStartContext;

enum class CpuStartError : u8 {
    NotSupported,
    InvalidHardwareId,
    InvalidEntryAddress,
    AlreadyStarted,
    Rejected,
};

[[nodiscard]] auto secondary_start_available() noexcept -> bool;
[[nodiscard]] auto start_secondary(
    kernel::CpuHardwareId hardware_id,
    CpuStartContext& context,
    const kernel::mm::DirectMap& direct_map) noexcept
    -> libk::Expected<void, CpuStartError>;

void initialize_cpu_entry(CpuEntryState& state, void* owner) noexcept;
[[nodiscard]] auto set_local_cpu_entry(CpuEntryState& state) noexcept -> bool;
[[nodiscard]] auto current_cpu_owner() noexcept -> void*;
void publish_active_stack(CpuEntryState& state, usize stack_top) noexcept;
[[nodiscard]] auto active_stack(const CpuEntryState& state) noexcept -> usize;
[[nodiscard]] auto trap_depth(const CpuEntryState& state) noexcept -> usize;
[[nodiscard]] auto trap_depth() noexcept -> usize;
[[nodiscard]] auto trap_entry_tick() noexcept -> u64;
void publish_panic_state(
    CpuEntryState& state,
    usize emergency_stack_top,
    void* panic_slot) noexcept;
[[nodiscard]] auto panic_slot(const CpuEntryState& state) noexcept -> void*;
[[nodiscard]] auto panic_slot() noexcept -> void*;
[[nodiscard]] auto emergency_stack(const CpuEntryState& state) noexcept -> usize;
[[nodiscard]] auto emergency_stack() noexcept -> usize;
[[nodiscard]] auto enter_emergency() noexcept -> bool;
void request_panic_stop(CpuEntryState& state) noexcept;
[[nodiscard]] auto panic_stop_requested() noexcept -> bool;
[[nodiscard]] auto interrupts_enabled() noexcept -> bool;

void wait_for_interrupt() noexcept;

using StackContinuation = void (*)(void*) noexcept;
using PanicContinuation = void (*)(void*) noexcept;

[[noreturn]] auto switch_to_stack_and_call(
    usize stack_top,
    void* argument,
    StackContinuation continuation) noexcept -> void;

[[noreturn]] auto switch_to_panic_stack(
    usize stack_top,
    void* argument,
    PanicContinuation continuation) noexcept -> void;

} // namespace arch
