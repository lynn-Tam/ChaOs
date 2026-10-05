#include <expected>
#include "arch/riscv64/cpu/start_context.hpp"
#include <sbi.hpp>
#include <arch/interrupt.hpp>
#include <arch/system.hpp>

#include <arch/cpu.hpp>
#include <libk/assert.hpp>
#include <base/types.hpp>

namespace arch::riscv64 {

void CpuStartContext::initialize(CpuHwId hardware_id, usize root, usize init_stack_top, CpuRuntime& runtime,
                                 SecondaryContinuation entry) noexcept {
    libk_assert(!ready());
    libk_assert(init_stack_top != 0);
    libk_assert((init_stack_top & 0xfU) == 0);
    libk_assert(entry != nullptr);

    hardware_id_ = hardware_id.raw;
    satp_ = root;
    init_stack_top_ = init_stack_top;
    runtime_ = &runtime;
    entry_ = entry;

    // Publishes every immutable payload field to the pre-C++ acquire in the
    // secondary entry. This gate is one-shot for the lifetime of CpuRuntime.
    publication_.store<libk::MemoryOrder::Release>(RISCV64_CPU_START_READY);
}

auto CpuStartContext::ready() const noexcept -> bool {
    return publication_.load<libk::MemoryOrder::Acquire>() == RISCV64_CPU_START_READY;
}

} // namespace arch::riscv64

namespace arch {

auto secondary_start_available() noexcept -> bool { return sbi::probe(sbi::Ext::Hsm); }

auto start_secondary(CpuHwId hardware_id, usize entry, usize record) noexcept
    -> std::expected<void, CpuStartError> {
    if (!record || (entry & 0x3U) != 0) return std::unexpected(CpuStartError::InvalidEntryAddress);

    const auto result = sbi::call(sbi::Ext::Hsm, 0, hardware_id.raw, entry, record);
    if (result) return {};
    switch (result.error()) {
    case sbi::Unsupported:
        return std::unexpected(CpuStartError::NotSupported);
    case sbi::Invalid:
        return std::unexpected(CpuStartError::InvalidHardwareId);
    case sbi::BadAddr:
        return std::unexpected(CpuStartError::InvalidEntryAddress);
    case sbi::Started:
    case sbi::Available:
        return std::unexpected(CpuStartError::AlreadyStarted);
    default:
        return std::unexpected(CpuStartError::Rejected);
    }
}

void wait_for_interrupt() noexcept { asm volatile("wfi" ::: "memory"); }

[[noreturn]] void halt_current_cpu([[maybe_unused]] HaltReason reason) noexcept {
    static_cast<void>(disable_interrupts());
    for (;;) wait_for_interrupt();
}
} // namespace arch
