#include <platform/cpu.hpp>

#include "arch/riscv64/sbi/base.hpp"
#include "arch/riscv64/sbi/hsm.hpp"

#include <core/debug.hpp>
#include <mm/direct_map.hpp>
#include <platform/memory_layout.hpp>

namespace platform {
namespace {

[[nodiscard]] constexpr auto start_error(isize error) noexcept
    -> CpuStartError {
    switch (error) {
    case arch::riscv64::sbi::not_supported:
        return CpuStartError::NotSupported;
    case arch::riscv64::sbi::invalid_parameter:
        return CpuStartError::InvalidHardwareId;
    case arch::riscv64::sbi::invalid_address:
        return CpuStartError::InvalidEntryAddress;
    case arch::riscv64::sbi::already_started:
    case arch::riscv64::sbi::already_available:
        return CpuStartError::AlreadyStarted;
    default:
        return CpuStartError::Rejected;
    }
}

static_assert(start_error(arch::riscv64::sbi::not_supported)
    == CpuStartError::NotSupported);
static_assert(start_error(arch::riscv64::sbi::invalid_parameter)
    == CpuStartError::InvalidHardwareId);
static_assert(start_error(arch::riscv64::sbi::invalid_address)
    == CpuStartError::InvalidEntryAddress);
static_assert(start_error(arch::riscv64::sbi::already_started)
    == CpuStartError::AlreadyStarted);
static_assert(start_error(arch::riscv64::sbi::denied)
    == CpuStartError::Rejected);

} // namespace

auto secondary_start_available() noexcept -> bool {
    return arch::riscv64::sbi::extension_available(
        arch::riscv64::sbi::hsm_extension_id);
}

auto start_secondary(
    kernel::CpuHardwareId hardware_id,
    arch::CpuStartContext& context,
    const kernel::mm::DirectMap& direct_map) noexcept
    -> libk::Expected<void, CpuStartError> {
    KASSERT(context.ready());

    constexpr usize entry = memory::secondary_entry_physical;
    const auto record = direct_map.unmap(
        kernel::mm::VirtAddr{reinterpret_cast<usize>(&context)}, sizeof(context));
    if (!record || (entry & 0x3U) != 0) {
        return libk::unexpected(CpuStartError::InvalidEntryAddress);
    }

    const auto result = arch::riscv64::sbi::hart_start(
        hardware_id.raw, entry, record.value().raw());
    if (result.error == arch::riscv64::sbi::success) {
        return libk::expected();
    }
    return libk::unexpected(start_error(result.error));
}

} // namespace platform
