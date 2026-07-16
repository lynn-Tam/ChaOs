#include "arch/riscv64/cpu/start_context.hpp"
#include "arch/riscv64/mmu/root_access.hpp"
#include "arch/riscv64/sbi/base.hpp"
#include "arch/riscv64/sbi/hsm.hpp"

#include <arch/cpu.hpp>
#include <core/kernel_image.hpp>
#include <core/debug.hpp>
#include <cpu/cpu_runtime.hpp>
#include <mm/direct_map.hpp>

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
    kernel::CpuHardwareId hardware_id,
    CpuStartContext& context,
    const kernel::mm::DirectMap& direct_map) noexcept
    -> libk::Expected<void, CpuStartError> {
    KASSERT(context.ready());

    const usize entry = kernel::image::secondary_entry().first().base().raw();
    const auto record = direct_map.unmap(
        kernel::mm::VirtAddr{reinterpret_cast<usize>(&context)}, sizeof(context));
    if (!record || (entry & 0x3U) != 0) {
        return libk::unexpected(CpuStartError::InvalidEntryAddress);
    }

    const auto result = riscv64::sbi::hart_start(
        hardware_id.raw, entry, record.value().raw());
    if (result.error == riscv64::sbi::success) {
        return libk::expected();
    }
    return libk::unexpected(start_error(result.error));
}

void wait_for_interrupt() noexcept {
    asm volatile("wfi" ::: "memory");
}

} // namespace arch
