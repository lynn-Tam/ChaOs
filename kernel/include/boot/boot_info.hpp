#pragma once

#include <core/types.hpp>
#include <libk/expected.hpp>
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

struct BootInfo final {
    FdtSource fdt{};
    TransitionMemory transition{};
    kernel::mm::RegionList memory_regions{};
};

enum class BootInfoError : uint8_t {
    InvalidFdt,
    InvalidStructure,
    InvalidMemoryMap,
    InvalidKernelRange,
    InvalidFdtRange,
};

[[nodiscard]] auto build_boot_info_from_fdt(
    BootInfo& destination,
    kernel::mm::PhysAddr fdt_physical,
    const void* fdt_view) noexcept
    -> libk::Expected<void, BootInfoError>;

} // namespace kernel::boot
