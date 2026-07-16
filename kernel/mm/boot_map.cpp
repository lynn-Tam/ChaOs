#include <mm/boot_map.hpp>

#include <libk/algorithm.hpp>
#include <libk/utility.hpp>

namespace kernel::mm {
namespace {

[[nodiscard]] constexpr auto priority(RegionKind kind) noexcept -> unsigned {
    switch (kind) {
    case RegionKind::KernelImage:
        return 3;
    case RegionKind::FirmwareReserved:
        return 2;
    case RegionKind::ReclaimableBootData:
        return 1;
    case RegionKind::AvailableRam:
    case RegionKind::Mmio:
        return 0;
    }
    return 0;
}

auto append(
    RegionList& map,
    PageRange range,
    RegionKind kind) noexcept -> bool {
    if (!map.empty()) {
        auto& last = map[map.size() - 1];
        const auto end = last.range.end_frame();
        // Reclaimable regions are lifetime authorities, not just page-state
        // runs. Adjacent FDT/initrd-style resources must remain separately
        // takeable by PMM even when they share the same region kind.
        if (kind != RegionKind::ReclaimableBootData
            && last.kind == kind
            && end
            && *end == range.first().frame()) {
            const auto next_end = range.end_frame();
            if (!next_end) {
                return false;
            }
            last.range = PageRange{
                last.range.first(),
                next_end->raw() - last.range.first().frame().raw(),
            };
            return true;
        }
    }
    return map.try_emplace_back(Region{range, kind});
}

} // namespace

auto BootMapBuilder::add_ram(PageRange range) noexcept
    -> libk::Expected<void, BootMapError> {
    if (!range.valid()) {
        return libk::unexpected(BootMapError::InvalidRange);
    }
    if (!ram_.try_push_back(range)) {
        return libk::unexpected(BootMapError::TooManyRamBanks);
    }
    return libk::expected();
}

auto BootMapBuilder::reserve(
    PageRange range,
    RegionKind kind) noexcept
    -> libk::Expected<void, BootMapError> {
    if (!range.valid()
        || (kind != RegionKind::FirmwareReserved
            && kind != RegionKind::KernelImage
            && kind != RegionKind::ReclaimableBootData)) {
        return libk::unexpected(BootMapError::InvalidRange);
    }
    if (!reservations_.try_emplace_back(Reservation{range, kind})) {
        return libk::unexpected(BootMapError::TooManyReservations);
    }
    return libk::expected();
}

auto BootMapBuilder::ram() const noexcept -> libk::Span<const PageRange> {
    return ram_.span();
}

auto BootMapBuilder::build_into(
    RegionList& destination) && noexcept -> BuildResult {
    destination.clear();
    const auto fail = [&destination](BootMapError error) noexcept
        -> BuildResult {
        destination.clear();
        return libk::unexpected(error);
    };

    if (ram_.empty()) {
        return fail(BootMapError::NoRam);
    }
    libk::insertion_sort(ram_, [](const auto& lhs, const auto& rhs) {
        return lhs.first() < rhs.first();
    });
    libk::insertion_sort(reservations_, [](const auto& lhs, const auto& rhs) {
        return lhs.range.first() < rhs.range.first();
    });

    for (size_t index = 1; index < ram_.size(); ++index) {
        if (ram_[index - 1].intersects(ram_[index])) {
            return fail(BootMapError::OverlappingRam);
        }
    }

    for (const auto& bank : ram_) {
        libk::InplaceVector<Page, 2 + 2 * max_reservations> boundaries{};
        if (!boundaries.try_push_back(bank.first())) {
            return fail(BootMapError::TooManyRegions);
        }
        const auto bank_end = bank.end_frame();
        if (!bank_end
            || !boundaries.try_push_back(Page{*bank_end})) {
            return fail(BootMapError::InvalidRange);
        }

        for (const auto& reservation : reservations_) {
            if (!bank.intersects(reservation.range)) {
                continue;
            }
            const auto reservation_end = reservation.range.end_frame();
            if (!reservation_end) {
                return fail(BootMapError::InvalidRange);
            }
            const Page first = reservation.range.first() < bank.first()
                ? bank.first()
                : reservation.range.first();
            const Pfn end_frame = *reservation_end < *bank_end
                ? *reservation_end
                : *bank_end;
            if (!boundaries.try_push_back(first)
                || !boundaries.try_push_back(Page{end_frame})) {
                return fail(BootMapError::TooManyRegions);
            }
        }

        libk::insertion_sort(boundaries, [](Page lhs, Page rhs) {
            return lhs < rhs;
        });

        size_t begin = 0;
        for (size_t end = 1; end < boundaries.size(); ++end) {
            if (boundaries[begin] == boundaries[end]) {
                continue;
            }

            RegionKind kind = RegionKind::AvailableRam;
            unsigned selected = 0;
            for (const auto& reservation : reservations_) {
                if (reservation.range.contains(boundaries[begin])
                    && priority(reservation.kind) > selected) {
                    kind = reservation.kind;
                    selected = priority(reservation.kind);
                }
            }

            const size_t pages = boundaries[end].frame().raw()
                - boundaries[begin].frame().raw();
            if (!append(destination, PageRange{boundaries[begin], pages}, kind)) {
                return fail(BootMapError::TooManyRegions);
            }
            begin = end;
        }
    }

    return libk::expected();
}

} // namespace kernel::mm
