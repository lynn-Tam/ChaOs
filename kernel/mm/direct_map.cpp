#include <mm/direct_map.hpp>

#include <core/debug.hpp>
#include <libk/checked_arithmetic.hpp>

namespace kernel::mm {

auto DirectMap::initialize_in(
    libk::ManualLifetime<DirectMap>& storage,
    const RegionList& memory,
    DirectMapLayout layout) noexcept -> InitResult {
    if (!layout.valid()) {
        return libk::unexpected(DirectMapError::InvalidLayout);
    }
    const auto physical_end =
        layout.physical_base.checked_add(layout.window_size);
    const auto virtual_end =
        layout.virtual_base.checked_add(layout.window_size);
    if (!physical_end || !virtual_end) {
        return libk::unexpected(DirectMapError::InvalidLayout);
    }

    DirectMap& map = storage.emplace(
        ConstructionKey{}, layout.physical_base,
        layout.virtual_base, layout.window_size);
    const auto initialized = map.initialize(memory);
    if (!initialized) {
        const DirectMapError error = initialized.error();
        storage.reset();
        return libk::unexpected(error);
    }
    return libk::expected();
}

auto DirectMap::initialize(const RegionList& memory) noexcept
    -> InitResult {
    const auto physical_end = physical_base_.checked_add(window_size_);
    KASSERT(physical_end);
    for (const Region& region : memory) {
        if (!region.valid()) {
            return libk::unexpected(DirectMapError::InvalidLayout);
        }
        if (!region.is_ram()) {
            continue;
        }
        const PageRange range = region.range;
        const auto end_frame = range.end_frame();
        if (!end_frame
            || range.first().base() < physical_base_
            || Page{*end_frame}.base() > *physical_end) {
            return libk::unexpected(DirectMapError::OutsideWindow);
        }

        if (ranges_.size() == ranges_.capacity()) {
            return libk::unexpected(DirectMapError::InvalidLayout);
        }
        auto* position = ranges_.begin();
        while (position != ranges_.end()
            && position->first() < range.first()) {
            ++position;
        }
        ranges_.insert(position, range);
    }

    if (ranges_.empty()) {
        return libk::unexpected(DirectMapError::InvalidLayout);
    }

    usize output = 0;
    for (usize input = 1; input < ranges_.size(); ++input) {
        PageRange& previous = ranges_[output];
        const PageRange current = ranges_[input];
        const auto previous_end = previous.end_frame();
        if (!previous_end || current.first().frame() < *previous_end) {
            return libk::unexpected(DirectMapError::InvalidLayout);
        }
        if (current.first().frame() == *previous_end) {
            const auto pages = libk::checked_add(
                previous.page_count(), current.page_count());
            if (!pages) {
                return libk::unexpected(DirectMapError::Overflow);
            }
            previous = PageRange{previous.first(), *pages};
            continue;
        }
        ++output;
        ranges_[output] = current;
    }
    while (ranges_.size() > output + 1) {
        KASSERT(ranges_.try_pop_back());
    }
    return libk::expected();
}

auto DirectMap::range(usize index) const noexcept -> PageRange {
    KASSERT(index < ranges_.size());
    return ranges_[index];
}

auto DirectMap::contains(PageRange range) const noexcept -> bool {
    if (!range.valid()) {
        return false;
    }
    for (const PageRange mapped : ranges_) {
        if (mapped.contains(range)) {
            return true;
        }
        if (range.first() < mapped.first()) {
            return false;
        }
    }
    return false;
}

auto DirectMap::map(PhysAddr address, usize size) const noexcept
    -> libk::Expected<VirtAddr, DirectMapError> {
    const auto covered = PageRange::covering_bytes(address, size);
    if (!covered) {
        return libk::unexpected(DirectMapError::Overflow);
    }
    if (!contains(*covered)) {
        return libk::unexpected(DirectMapError::NotMapped);
    }
    const auto offset = physical_base_.checked_distance_to(address);
    if (!offset || *offset > window_size_ || size > window_size_ - *offset) {
        return libk::unexpected(DirectMapError::OutsideWindow);
    }
    const auto mapped = virtual_base_.checked_add(*offset);
    if (!mapped) {
        return libk::unexpected(DirectMapError::Overflow);
    }
    return libk::expected(*mapped);
}

auto DirectMap::unmap(VirtAddr address, usize size) const noexcept
    -> libk::Expected<PhysAddr, DirectMapError> {
    const auto offset = virtual_base_.checked_distance_to(address);
    if (!offset || *offset > window_size_ || size > window_size_ - *offset) {
        return libk::unexpected(DirectMapError::OutsideWindow);
    }
    const auto physical = physical_base_.checked_add(*offset);
    if (!physical) {
        return libk::unexpected(DirectMapError::Overflow);
    }
    const auto covered = PageRange::covering_bytes(*physical, size);
    if (!covered || !contains(*covered)) {
        return libk::unexpected(DirectMapError::NotMapped);
    }
    return libk::expected(*physical);
}

} // namespace kernel::mm
