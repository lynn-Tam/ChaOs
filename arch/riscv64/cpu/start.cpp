#include "arch/riscv64/cpu/start_context.hpp"
#include "arch/riscv64/mmu/root_access.hpp"

#include <arch/cpu.hpp>
#include <core/debug.hpp>
#include <cpu/cpu_runtime.hpp>

namespace arch::riscv64 {

void CpuStartContext::initialize(
    kernel::CpuHardwareId hardware_id,
    RootToken root,
    usize init_stack_top,
    kernel::CpuRuntime& runtime,
    SecondaryContinuation entry) noexcept {
    KASSERT(!ready());
    KASSERT(init_stack_top != 0);
    KASSERT((init_stack_top & 0xfU) == 0);
    KASSERT(entry != nullptr);

    hardware_id_ = hardware_id.raw;
    satp_ = RootAccess::value(root);
    init_stack_top_ = init_stack_top;
    runtime_ = &runtime;
    entry_ = entry;

    // Publishes every immutable payload field to the pre-C++ acquire in the
    // secondary entry. This gate is one-shot for the lifetime of CpuRuntime.
    publication_.store<libk::MemoryOrder::Release>(RISCV64_CPU_START_READY);
}

auto CpuStartContext::ready() const noexcept -> bool {
    return publication_.load<libk::MemoryOrder::Acquire>()
        == RISCV64_CPU_START_READY;
}

} // namespace arch::riscv64

namespace arch {

void wait_for_interrupt() noexcept {
    asm volatile("wfi" ::: "memory");
}

} // namespace arch
