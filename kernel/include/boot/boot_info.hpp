#pragma once

#include <core/types.hpp>
#include <cpu/topology.hpp>
#include <libk/expected.hpp>
#include <libk/inplace_vector.hpp>
#include <libk/optional.hpp>
#include <mm/boot_map.hpp>

namespace kernel::boot {

struct FdtSource final {
    kernel::mm::PhysAddr physical{};
    u32 size{};
    kernel::mm::PageRange pages{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return size != 0 && pages.valid();
    }
};

struct TransitionMemory final {
    kernel::mm::PageRange pages{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return pages.valid();
    }
};

enum class BootModuleKind : u8 {
    Bundle,
};

struct BootModule final {
    kernel::mm::PhysAddr physical{};
    usize size{};
    kernel::mm::PageRange pages{};
    BootModuleKind kind{BootModuleKind::Bundle};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return size != 0 && pages.valid();
    }
};

struct BootCpu final {
    CpuHardwareId hardware_id{};
    CpuAvailability availability{CpuAvailability::Disabled};
};

// Firmware CPU identifiers and availability are normalized while the boot
// protocol is still in scope. Kernel initialization consumes this value and
// never needs to reinterpret the firmware tree.
struct CpuHandoff final {
    libk::InplaceVector<BootCpu, kernel::max_cpu_count> cpus{};
    usize boot_index{};

    [[nodiscard]] auto summary() const noexcept -> CpuTopologySummary {
        return CpuTopologySummary{
            .count = cpus.size(),
            .boot_index = boot_index,
        };
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return !cpus.empty() && boot_index < cpus.size();
    }
};

struct BootInfo final {
    FdtSource fdt{};
    TransitionMemory transition{};
    libk::optional<BootModule> module{};
    CpuHandoff cpu{};
    u64 timebase_frequency{};
    kernel::mm::RegionList memory_regions{};
};

enum class BootInfoError : uint8_t {
    InvalidFdt,
    InvalidStructure,
    InvalidMemoryMap,
    InvalidKernelRange,
    InvalidFdtRange,
    InvalidModuleRange,
    InvalidCpuTopology,
    InvalidTimebase,
};

[[nodiscard]] auto build_boot_info_from_fdt(
    BootInfo& destination,
    CpuHardwareId boot_cpu,
    kernel::mm::PhysAddr fdt_physical,
    const void* fdt_view) noexcept
    -> libk::Expected<void, BootInfoError>;

} // namespace kernel::boot
