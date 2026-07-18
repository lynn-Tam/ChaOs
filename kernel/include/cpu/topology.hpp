#pragma once

#include <core/types.hpp>

namespace kernel {

// Firmware IDs remain opaque and sparse, while every normalized topology is
// bounded before it enters kernel initialization.
inline constexpr usize max_cpu_count = 256;

struct CpuId final {
    usize raw{};

    friend constexpr auto operator==(CpuId lhs, CpuId rhs) noexcept -> bool {
        return lhs.raw == rhs.raw;
    }
};

// Firmware CPU identifiers are opaque and may be sparse.
struct CpuHardwareId final {
    usize raw{};

    friend constexpr auto operator==(
        CpuHardwareId lhs,
        CpuHardwareId rhs) noexcept -> bool {
        return lhs.raw == rhs.raw;
    }
};

enum class CpuAvailability : u8 {
    Enabled,
    Disabled,
    Failed,
};

struct CpuTopologySummary final {
    usize count{};
    usize boot_index{};
};

} // namespace kernel
