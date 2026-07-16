// arch/riscv64/cpu/local_entry.cpp
// Binds the selected CPU entry block to RISC-V sscratch.

#include <arch/cpu.hpp>
#include <arch/interrupt.hpp>

#include "arch/riscv64/cpu/csr.hpp"

#include <core/debug.hpp>

namespace arch {
namespace {

[[nodiscard]] auto local_state() noexcept -> CpuEntryState* {
    return reinterpret_cast<CpuEntryState*>(
        riscv64::Sscratch::read());
}

} // namespace

void initialize_cpu_entry(CpuEntryState& state, void* owner) noexcept {
    KASSERT(owner != nullptr);
    state.initialize(owner);
}

auto set_local_cpu_entry(CpuEntryState& state) noexcept -> bool {
    KASSERT(state.owner_cpu != nullptr);

    riscv64::Sscratch::write(reinterpret_cast<usize>(&state));
    return riscv64::Sscratch::read() == reinterpret_cast<usize>(&state);
}

auto current_cpu_owner() noexcept -> void* {
    const auto* const state = local_state();
    return state != nullptr ? state->owner_cpu : nullptr;
}

void publish_active_stack(CpuEntryState& state, usize stack_top) noexcept {
    KASSERT(stack_top != 0);
    KASSERT((stack_top & 0xfU) == 0);
    state.publish_active_stack(stack_top);
}

auto active_stack(const CpuEntryState& state) noexcept -> usize {
    return state.active_stack_top;
}

auto trap_depth(const CpuEntryState& state) noexcept -> usize {
    return state.trap_depth;
}

auto trap_depth() noexcept -> usize {
    const auto* const state = local_state();
    KASSERT(state != nullptr);
    return trap_depth(*state);
}

void publish_panic_state(
    CpuEntryState& state,
    usize emergency_stack_top,
    void* slot) noexcept {
    KASSERT(emergency_stack_top != 0);
    KASSERT((emergency_stack_top & 0xfU) == 0);
    KASSERT(slot != nullptr);
    state.publish_diagnostics(emergency_stack_top, slot);
}

auto panic_slot(const CpuEntryState& state) noexcept -> void* {
    return state.panic_slot;
}

auto panic_slot() noexcept -> void* {
    const auto* const state = local_state();
    return state != nullptr ? panic_slot(*state) : nullptr;
}

auto emergency_stack(const CpuEntryState& state) noexcept -> usize {
    return state.emergency_stack_top;
}

auto emergency_stack() noexcept -> usize {
    const auto* const state = local_state();
    return state != nullptr ? emergency_stack(*state) : 0;
}

auto enter_emergency() noexcept -> bool {
    auto* const state = local_state();
    if (state == nullptr || state->emergency_depth != 0) {
        return false;
    }
    state->emergency_depth = 1;
    return true;
}

void request_panic_stop(CpuEntryState& state) noexcept {
    __atomic_store_n(
        &state.panic_stop_requested, usize{1}, __ATOMIC_RELEASE);
}

auto panic_stop_requested() noexcept -> bool {
    const auto* const state = local_state();
    return state != nullptr
        && __atomic_load_n(
            &state->panic_stop_requested, __ATOMIC_ACQUIRE) != 0;
}

auto interrupts_enabled() noexcept -> bool {
    return riscv64::Sstatus::is_interrupts_enabled();
}

auto disable_interrupts() noexcept -> InterruptState {
    const usize previous =
        riscv64::Sstatus::read_and_clear_bits(riscv64::Sstatus::SIE);
    return InterruptState{(previous & riscv64::Sstatus::SIE) != 0};
}

void enable_interrupts() noexcept {
    riscv64::Sstatus::enable_interrupts();
}

void restore_interrupts(InterruptState state) noexcept {
    if (state.enabled()) {
        riscv64::Sstatus::enable_interrupts();
    } else {
        riscv64::Sstatus::disable_interrupts();
    }
}

[[noreturn]] auto switch_to_stack_and_call(
    usize stack_top,
    kernel::KernelState& kernel,
    kernel::CpuRuntime& runtime,
    StackContinuation continuation) noexcept -> void {
    KASSERT(stack_top != 0);
    KASSERT((stack_top & 0xfU) == 0);
    KASSERT(continuation != nullptr);

    asm volatile(
        "mv sp, %[stack]\n"
        "mv a0, %[kernel]\n"
        "mv a1, %[runtime]\n"
        "jr %[target]\n"
        :
        : [stack] "r"(stack_top),
          [kernel] "r"(&kernel),
          [runtime] "r"(&runtime),
          [target] "r"(continuation)
        : "a0", "a1", "memory");
    __builtin_unreachable();
}

[[noreturn]] auto switch_to_panic_stack(
    usize stack_top,
    void* argument,
    PanicContinuation continuation) noexcept -> void {
    if (stack_top == 0 || (stack_top & 0xfU) != 0
        || continuation == nullptr) {
        for (;;) {
            asm volatile("wfi");
        }
    }
    asm volatile(
        "mv sp, %[stack]\n"
        "mv a0, %[argument]\n"
        "jr %[target]\n"
        :
        : [stack] "r"(stack_top),
          [argument] "r"(argument),
          [target] "r"(continuation)
        : "a0", "memory");
    __builtin_unreachable();
}

} // namespace arch
