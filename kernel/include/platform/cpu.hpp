#pragma once

#include <arch/cpu.hpp>
#include <cpu/topology.hpp>
#include <libk/expected.hpp>

namespace kernel::mm {
class DirectMap;
}

namespace platform {

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
    arch::CpuStartContext& context,
    const kernel::mm::DirectMap& direct_map) noexcept
    -> libk::Expected<void, CpuStartError>;

} // namespace platform
