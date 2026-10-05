#include <expected>
#include "arch/riscv64/cpu/start_context.hpp"
#include "arch/riscv64/sbi/base.hpp"
#include "arch/riscv64/sbi/hsm.hpp"

#include <arch/cpu.hpp>
#include <boot/link.hpp>
#include <libk/assert.hpp>
#include <base/types.hpp>
#include <cpu/runtime.hpp>
#include <mm/phys.hpp>

namespace arch::riscv64 {

void CpuStartContext::initialize(
    CpuHwId hardware_id,
    usize root,
    usize init_stack_top,
    CpuRuntime& runtime,
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
    return publication_.load<libk::MemoryOrder::Acquire>()
        == RISCV64_CPU_START_READY;
}

} // namespace arch::riscv64

namespace arch {

namespace {

[[nodiscard]] constexpr auto start_error(isize error) noexcept
    -> CpuStartError {
    switch (error) {
    case riscv64::sbi::not_supported:
        return CpuStartError::NotSupported;
    case riscv64::sbi::invalid_parameter:
        return CpuStartError::InvalidHardwareId;
    case riscv64::sbi::invalid_address:
        return CpuStartError::InvalidEntryAddress;
    case riscv64::sbi::already_started:
    case riscv64::sbi::already_available:
        return CpuStartError::AlreadyStarted;
    default:
        return CpuStartError::Rejected;
    }
}

} // namespace

auto secondary_start_available() noexcept -> bool {
    return riscv64::sbi::extension_available(riscv64::sbi::hsm_extension_id);
}

auto start_secondary(
    CpuHwId hardware_id,
    CpuStartContext& context,
    const mm::DirectMap& direct_map) noexcept
    -> std::expected<void, CpuStartError> {
    libk_assert(context.ready());

    const usize entry = secondary_pages().base().base().raw();
    const auto record = direct_map.unmap(
        mm::Virt{reinterpret_cast<usize>(&context)}, sizeof(context));
    if (!record || (entry & 0x3U) != 0) {
        return std::unexpected(CpuStartError::InvalidEntryAddress);
    }

    const auto result = riscv64::sbi::hart_start(
        hardware_id.raw, entry, record.value().raw());
    if (result.error == riscv64::sbi::success) {
        return {};
    }
    return std::unexpected(start_error(result.error));
}

void wait_for_interrupt() noexcept {
    asm volatile("wfi" ::: "memory");
}

} // namespace arch
