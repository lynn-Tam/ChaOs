#pragma once

#include <boot/info.hpp>
#include <boot/fdt.hpp>
#include <expected>

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
    const Fdt& tree,
    CpuHwId boot_hardware_id,
    CpuHandoff& destination) noexcept
    -> std::expected<void, CpuTopologyError>;

