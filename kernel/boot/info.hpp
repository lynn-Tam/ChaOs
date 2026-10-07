#pragma once

#include <base/types.hpp>
#include <cpu/types.hpp>
#include <expected>
#include <libk/inplace_vector.hpp>
#include <optional>
#include <mm/table.hpp>
#include <ranges>

// Firmware input may overlap; finish sweeps boundaries without another index.
class BootMap {
  public:
    enum class Err : u8 { Invalid, NoRam, Overlap, Capacity };
    auto add_ram(mm::Pages r) noexcept -> std::expected<void, Err>;
    auto reserve(mm::Pages r, mm::Region::Kind kind) noexcept -> std::expected<void, Err>;
    auto ram() const noexcept {
        return input_.span() |
               std::views::filter([](const mm::Region& r) { return r.kind == mm::Region::Kind::Ram; }) |
               std::views::transform([](const mm::Region& r) { return r.range; });
    }
    auto finish(mm::RegionList& out) && noexcept -> std::expected<void, Err>;

  private:
    mm::RegionList input_{};
};

struct FdtSource final {
    mm::Phys physical{};
    u32 size{};
    mm::Pages pages{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return size != 0 && pages.valid();
    }
};

struct BootModule final {
    mm::Phys physical{};
    usize size{};
    mm::Pages pages{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return size != 0 && pages.valid();
    }
};

struct BootCpu final {
    CpuHwId hardware_id{};
    CpuAvail availability{CpuAvail::Disabled};
};

// Firmware CPU identifiers and availability are normalized while the boot
// protocol is still in scope. Kernel initialization consumes this value and
// never needs to reinterpret the firmware tree.
struct CpuHandoff final {
    libk::InplaceVector<BootCpu, MaxCpus> cpus{};
    usize boot_index{};

    [[nodiscard]] auto summary() const noexcept -> CpuTopo {
        return CpuTopo{
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
    mm::Pages transition{};
    std::optional<BootModule> module{};
    CpuHandoff cpu{};
    u64 timebase_frequency{};
    // Optional system RISC-V IOMMU register range normalized from /soc.
    std::optional<mm::Pages> iommu{};
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
    mm::RegionList& memory,
    CpuHwId boot_cpu,
    mm::Phys fdt_physical,
    const void* fdt_view) noexcept
    -> std::expected<void, BootInfoError>;


// Boot assembles permanent mappings before exposing the hardware root.
auto kernel_root(mm::Pmm&) noexcept -> std::expected<mm::PageTable, mm::PtErr>;

class KernelState;
struct CpuRuntime;
enum class BootErr : u8 {
    InvalidModule, InvalidBundle, Ownership, OutOfMemory,
    InvalidState, MappingFailed, CapabilityFailed, SchedulingFailed,
};
auto boot_root(KernelState&, CpuRuntime&, BootModule, mm::BootPages&&) noexcept
    -> std::expected<void, BootErr>;
