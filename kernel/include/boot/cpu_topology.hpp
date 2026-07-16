#pragma once

#include <boot/firmware/devicetree/fdt.hpp>
#include <cpu/cpu_registry.hpp>
#include <cpu/topology.hpp>
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
    RegistryRejected,
};

using CpuSummaryResult =
    libk::Expected<CpuTopologySummary, CpuTopologyError>;
using CpuPopulateResult = libk::Expected<void, CpuTopologyError>;

[[nodiscard]] auto parse_fdt_cpus_summary(
    const kernel::boot::fdt::FDT_View& view,
    CpuHardwareId boot_hardware_id) noexcept -> CpuSummaryResult;

[[nodiscard]] auto populate_fdt_cpus(
    const kernel::boot::fdt::FDT_View& view,
    CpuRegistry::Builder& builder) noexcept -> CpuPopulateResult;

} // namespace kernel::boot
