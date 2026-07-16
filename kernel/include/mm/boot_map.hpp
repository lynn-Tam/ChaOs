#pragma once

#include <stddef.h>
#include <stdint.h>

#include <libk/expected.hpp>
#include <libk/inplace_vector.hpp>
#include <libk/span.hpp>
#include <mm/addr.hpp>

namespace kernel::mm {

enum class RegionKind : uint8_t {
    AvailableRam,
    ReclaimableBootData,
    KernelImage,
    FirmwareReserved,
    Mmio,
};

struct Region {
    PageRange range{};
    RegionKind kind{RegionKind::Mmio};

    [[nodiscard]] constexpr auto valid() const noexcept -> bool {
        return range.valid();
    }

    [[nodiscard]] constexpr auto is_ram() const noexcept -> bool {
        return kind != RegionKind::Mmio;
    }

    [[nodiscard]] constexpr auto is_reclaimable() const noexcept -> bool {
        return kind == RegionKind::ReclaimableBootData;
    }
};

inline constexpr size_t max_regions = 32;
using RegionList = libk::InplaceVector<Region, max_regions>;

enum class BootMapError : uint8_t {
    InvalidRange,
    NoRam,
    TooManyRamBanks,
    TooManyReservations,
    OverlappingRam,
    TooManyRegions,
};

class BootMapBuilder {
public:
    using BuildResult = libk::Expected<void, BootMapError>;

    [[nodiscard]] auto add_ram(PageRange range) noexcept
        -> libk::Expected<void, BootMapError>;
    [[nodiscard]] auto reserve(
        PageRange range,
        RegionKind kind) noexcept
        -> libk::Expected<void, BootMapError>;

    [[nodiscard]] auto ram() const noexcept -> libk::Span<const PageRange>;
    [[nodiscard]] auto build_into(RegionList& destination) && noexcept
        -> BuildResult;

private:
    static constexpr size_t max_ram_banks = 8;
    static constexpr size_t max_reservations = 16;

    struct Reservation {
        PageRange range{};
        RegionKind kind{RegionKind::FirmwareReserved};
    };

    libk::InplaceVector<PageRange, max_ram_banks> ram_{};
    libk::InplaceVector<Reservation, max_reservations> reservations_{};
};

} // namespace kernel::mm
