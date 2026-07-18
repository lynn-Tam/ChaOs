#pragma once

#include <boot/boot_info.hpp>
#include <boot/firmware/devicetree/fdt.hpp>
#include <libk/expected.hpp>

namespace kernel::boot {

enum class CpuTopologyError : u8 {
    MissingCpusNode,
    InvalidAddressCells,
    InvalidSizeCells,
    InvalidCpuNode,
    MissingReg,
    InvalidReg,
    InvalidStatus,
    BootCpuMissing,
    BootCpuUnavailable,
    DuplicateBootCpu,
    DuplicateCpu,
    CapacityExceeded,
};

[[nodiscard]] auto parse_fdt_cpus(
    const kernel::boot::fdt::FDT_View& view,
    CpuHardwareId boot_hardware_id,
    CpuHandoff& destination) noexcept
    -> libk::Expected<void, CpuTopologyError>;

} // namespace kernel::boot
