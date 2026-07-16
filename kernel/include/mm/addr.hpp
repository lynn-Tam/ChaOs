#pragma once

#include <stddef.h>
#include <stdint.h>

#include <libk/optional.hpp>
#include <libk/limits.hpp>

namespace kernel::mm {

inline constexpr size_t page_size = 4096;

static_assert(page_size != 0);
static_assert((page_size & (page_size - 1)) == 0);

class Pfn {
public:
    constexpr Pfn() noexcept = default;
    constexpr explicit Pfn(uintptr_t value) noexcept : value_(value) {}

    [[nodiscard]] constexpr auto raw() const noexcept -> uintptr_t {
        return value_;
    }

    [[nodiscard]] constexpr auto checked_add(size_t pages) const noexcept
        -> libk::optional<Pfn> {
        if (pages > libk::numeric_limits<uintptr_t>::max() - value_) {
            return libk::nullopt;
        }
        return Pfn{value_ + pages};
    }

    friend constexpr auto operator==(Pfn a, Pfn b) noexcept -> bool {
        return a.value_ == b.value_;
    }
    friend constexpr auto operator!=(Pfn a, Pfn b) noexcept -> bool {
        return !(a == b);
    }
    friend constexpr auto operator<(Pfn a, Pfn b) noexcept -> bool {
        return a.value_ < b.value_;
    }
    friend constexpr auto operator<=(Pfn a, Pfn b) noexcept -> bool {
        return a.value_ <= b.value_;
    }
    friend constexpr auto operator>(Pfn a, Pfn b) noexcept -> bool {
        return a.value_ > b.value_;
    }
    friend constexpr auto operator>=(Pfn a, Pfn b) noexcept -> bool {
        return a.value_ >= b.value_;
    }

private:

    uintptr_t value_{};
};

class PhysAddr {
public:
    constexpr PhysAddr() noexcept = default;
    constexpr explicit PhysAddr(uintptr_t value) noexcept : value_(value) {}

    [[nodiscard]] constexpr auto raw() const noexcept -> uintptr_t {
        return value_;
    }

    [[nodiscard]] constexpr auto is_aligned(size_t alignment) const noexcept -> bool {
        return alignment != 0 && value_ % alignment == 0;
    }

    [[nodiscard]] constexpr auto checked_add(size_t bytes) const noexcept
        -> libk::optional<PhysAddr> {
        if (bytes > libk::numeric_limits<uintptr_t>::max() - value_) {
            return libk::nullopt;
        }
        return PhysAddr{value_ + bytes};
    }

    [[nodiscard]] constexpr auto checked_sub(size_t bytes) const noexcept
        -> libk::optional<PhysAddr> {
        if (bytes > value_) {
            return libk::nullopt;
        }
        return PhysAddr{value_ - bytes};
    }

    [[nodiscard]] constexpr auto aligned_down(size_t alignment) const noexcept
        -> libk::optional<PhysAddr> {
        if (alignment == 0) {
            return libk::nullopt;
        }
        return PhysAddr{value_ - (value_ % alignment)};
    }

    [[nodiscard]] constexpr auto aligned_up(size_t alignment) const noexcept
        -> libk::optional<PhysAddr> {
        if (alignment == 0) {
            return libk::nullopt;
        }
        const size_t remainder = value_ % alignment;
        return remainder == 0 ? libk::optional<PhysAddr>{*this}
                              : checked_add(alignment - remainder);
    }

    [[nodiscard]] constexpr auto checked_distance_to(PhysAddr end) const noexcept
        -> libk::optional<size_t> {
        if (end.value_ < value_) {
            return libk::nullopt;
        }
        return static_cast<size_t>(end.value_ - value_);
    }

    friend constexpr auto operator==(PhysAddr a, PhysAddr b) noexcept -> bool {
        return a.value_ == b.value_;
    }
    friend constexpr auto operator!=(PhysAddr a, PhysAddr b) noexcept -> bool {
        return !(a == b);
    }
    friend constexpr auto operator<(PhysAddr a, PhysAddr b) noexcept -> bool {
        return a.value_ < b.value_;
    }
    friend constexpr auto operator<=(PhysAddr a, PhysAddr b) noexcept -> bool {
        return a.value_ <= b.value_;
    }
    friend constexpr auto operator>(PhysAddr a, PhysAddr b) noexcept -> bool {
        return a.value_ > b.value_;
    }
    friend constexpr auto operator>=(PhysAddr a, PhysAddr b) noexcept -> bool {
        return a.value_ >= b.value_;
    }

private:
    uintptr_t value_{};
};

class Page {
public:
    constexpr Page() noexcept = default;
    constexpr explicit Page(Pfn frame) noexcept : frame_(frame) {}

    [[nodiscard]] static constexpr auto from_base(PhysAddr address) noexcept
        -> libk::optional<Page> {
        if (!address.is_aligned(page_size)) {
            return libk::nullopt;
        }
        return Page{Pfn{address.raw() / page_size}};
    }

    [[nodiscard]] constexpr auto frame() const noexcept -> Pfn {
        return frame_;
    }

    [[nodiscard]] constexpr auto valid() const noexcept -> bool {
        return frame_.raw() <= max_frame_number();
    }

    [[nodiscard]] constexpr auto base() const noexcept -> PhysAddr {
        return PhysAddr{frame_.raw() * page_size};
    }

    friend constexpr auto operator==(Page a, Page b) noexcept -> bool {
        return a.frame_ == b.frame_;
    }
    friend constexpr auto operator!=(Page a, Page b) noexcept -> bool {
        return !(a == b);
    }
    friend constexpr auto operator<(Page a, Page b) noexcept -> bool {
        return a.frame_ < b.frame_;
    }

private:
    [[nodiscard]] static constexpr auto max_frame_number() noexcept -> uintptr_t {
        return ~static_cast<uintptr_t>(0) / page_size;
    }

    Pfn frame_{};
};

class PageIter {
public:
    constexpr explicit PageIter(Pfn frame) noexcept : frame_(frame) {}

    constexpr auto operator++() noexcept -> PageIter& {
        frame_ = *frame_.checked_add(1);
        return *this;
    }

    [[nodiscard]] constexpr auto operator*() const noexcept -> Page {
        return Page{frame_};
    }

    friend constexpr auto operator==(PageIter a, PageIter b) noexcept -> bool {
        return a.frame_ == b.frame_;
    }
    friend constexpr auto operator!=(PageIter a, PageIter b) noexcept -> bool {
        return !(a == b);
    }

private:
    Pfn frame_{};
};

class PageRange {
public:
    constexpr PageRange() noexcept = default;
    constexpr PageRange(Page first, size_t page_count) noexcept
        : first_(first), page_count_(page_count) {}

    [[nodiscard]] static constexpr auto from_aligned_bytes(
        PhysAddr first,
        size_t byte_size) noexcept -> libk::optional<PageRange> {
        const auto page = Page::from_base(first);
        if (!page || byte_size == 0 || byte_size % page_size != 0) {
            return libk::nullopt;
        }
        const PageRange range{*page, byte_size / page_size};
        return range.valid() ? libk::optional<PageRange>{range} : libk::nullopt;
    }

    [[nodiscard]] static constexpr auto covering_bytes(
        PhysAddr first,
        size_t byte_size) noexcept -> libk::optional<PageRange> {
        if (byte_size == 0) {
            return libk::nullopt;
        }
        const auto end = first.checked_add(byte_size);
        const auto aligned_first = first.aligned_down(page_size);
        const auto aligned_end = end ? end->aligned_up(page_size) : libk::nullopt;
        if (!aligned_first || !aligned_end) {
            return libk::nullopt;
        }
        const auto size = aligned_first->checked_distance_to(*aligned_end);
        return size ? from_aligned_bytes(*aligned_first, *size) : libk::nullopt;
    }

    [[nodiscard]] static constexpr auto contained_bytes(
        PhysAddr first,
        size_t byte_size) noexcept -> libk::optional<PageRange> {
        if (byte_size == 0) {
            return libk::nullopt;
        }
        const auto end = first.checked_add(byte_size);
        const auto aligned_first = first.aligned_up(page_size);
        const auto aligned_end = end ? end->aligned_down(page_size) : libk::nullopt;
        if (!aligned_first || !aligned_end) {
            return libk::nullopt;
        }
        const auto size = aligned_first->checked_distance_to(*aligned_end);
        return size && *size != 0
            ? from_aligned_bytes(*aligned_first, *size)
            : libk::nullopt;
    }

    [[nodiscard]] constexpr auto valid() const noexcept -> bool {
        if (!first_.valid() || page_count_ == 0 || page_count_ > max_page_count()) {
            return false;
        }
        const uintptr_t max_frame = ~static_cast<uintptr_t>(0) / page_size;
        return page_count_ - 1 <= max_frame - first_.frame().raw();
    }

    [[nodiscard]] constexpr auto first() const noexcept -> Page {
        return first_;
    }

    [[nodiscard]] constexpr auto page_count() const noexcept -> size_t {
        return page_count_;
    }

    [[nodiscard]] constexpr auto byte_size() const noexcept -> size_t {
        return page_count_ * page_size;
    }

    [[nodiscard]] constexpr auto end_frame() const noexcept -> libk::optional<Pfn> {
        return first_.frame().checked_add(page_count_);
    }

    [[nodiscard]] constexpr auto contains(Page page) const noexcept -> bool {
        const auto end = end_frame();
        return valid() && end && page.frame() >= first_.frame() && page.frame() < *end;
    }

    [[nodiscard]] constexpr auto contains(PageRange other) const noexcept -> bool {
        const auto end = end_frame();
        const auto other_end = other.end_frame();
        return valid() && other.valid() && end && other_end
            && other.first_.frame() >= first_.frame() && *other_end <= *end;
    }

    [[nodiscard]] constexpr auto intersects(PageRange other) const noexcept -> bool {
        const auto end = end_frame();
        const auto other_end = other.end_frame();
        return valid() && other.valid() && end && other_end
            && first_.frame() < *other_end && other.first_.frame() < *end;
    }

    [[nodiscard]] constexpr auto begin() const noexcept -> PageIter {
        return PageIter{first_.frame()};
    }

    [[nodiscard]] constexpr auto end() const noexcept -> PageIter {
        return PageIter{*end_frame()};
    }

private:
    [[nodiscard]] static constexpr auto max_page_count() noexcept -> size_t {
        return ~static_cast<size_t>(0) / page_size;
    }

    Page first_{};
    size_t page_count_{};
};

class VirtAddr {
public:
    constexpr VirtAddr() noexcept = default;
    constexpr explicit VirtAddr(uintptr_t value) noexcept : value_(value) {}

    [[nodiscard]] constexpr auto raw() const noexcept -> uintptr_t {
        return value_;
    }

    [[nodiscard]] static constexpr auto null() noexcept -> VirtAddr {
        return VirtAddr{};
    }

    [[nodiscard]] constexpr auto is_null() const noexcept -> bool {
        return value_ == 0;
    }

    [[nodiscard]] constexpr auto valid() const noexcept -> bool {
        return !is_null();
    }

    [[nodiscard]] constexpr auto is_aligned(size_t alignment) const noexcept -> bool {
        return alignment != 0 && value_ % alignment == 0;
    }

    [[nodiscard]] constexpr auto checked_add(size_t offset) const noexcept
        -> libk::optional<VirtAddr> {
        if (offset > libk::numeric_limits<uintptr_t>::max() - value_) {
            return libk::nullopt;
        }
        return VirtAddr{value_ + offset};
    }

    [[nodiscard]] constexpr auto checked_sub(size_t offset) const noexcept
        -> libk::optional<VirtAddr> {
        if (offset > value_) {
            return libk::nullopt;
        }
        return VirtAddr{value_ - offset};
    }

    [[nodiscard]] constexpr auto aligned_down(size_t alignment) const noexcept
        -> libk::optional<VirtAddr> {
        if (alignment == 0) {
            return libk::nullopt;
        }
        return VirtAddr{value_ - (value_ % alignment)};
    }

    [[nodiscard]] constexpr auto aligned_up(size_t alignment) const noexcept
        -> libk::optional<VirtAddr> {
        if (alignment == 0) {
            return libk::nullopt;
        }
        const size_t remainder = value_ % alignment;
        return remainder == 0 ? libk::optional<VirtAddr>{*this}
                              : checked_add(alignment - remainder);
    }

    [[nodiscard]] constexpr auto checked_distance_to(VirtAddr end) const noexcept
        -> libk::optional<size_t> {
        if (end.value_ < value_) {
            return libk::nullopt;
        }
        return static_cast<size_t>(end.value_ - value_);
    }

    friend constexpr auto operator==(VirtAddr a, VirtAddr b) noexcept -> bool {
        return a.value_ == b.value_;
    }
    friend constexpr auto operator!=(VirtAddr a, VirtAddr b) noexcept -> bool {
        return !(a == b);
    }
    friend constexpr auto operator<(VirtAddr a, VirtAddr b) noexcept -> bool {
        return a.value_ < b.value_;
    }
    friend constexpr auto operator<=(VirtAddr a, VirtAddr b) noexcept -> bool {
        return a.value_ <= b.value_;
    }
    friend constexpr auto operator>(VirtAddr a, VirtAddr b) noexcept -> bool {
        return a.value_ > b.value_;
    }
    friend constexpr auto operator>=(VirtAddr a, VirtAddr b) noexcept -> bool {
        return a.value_ >= b.value_;
    }

private:
    uintptr_t value_{};
};

class Vpn {
public:
    constexpr Vpn() noexcept = default;
    constexpr explicit Vpn(uintptr_t value) noexcept : value_(value) {}

    [[nodiscard]] constexpr auto raw() const noexcept -> uintptr_t {
        return value_;
    }

    [[nodiscard]] constexpr auto checked_add(size_t pages) const noexcept
        -> libk::optional<Vpn> {
        if (pages > libk::numeric_limits<uintptr_t>::max() - value_) {
            return libk::nullopt;
        }
        return Vpn{value_ + pages};
    }

    friend constexpr auto operator==(Vpn lhs, Vpn rhs) noexcept -> bool {
        return lhs.value_ == rhs.value_;
    }
    friend constexpr auto operator!=(Vpn lhs, Vpn rhs) noexcept -> bool {
        return !(lhs == rhs);
    }
    friend constexpr auto operator<(Vpn lhs, Vpn rhs) noexcept -> bool {
        return lhs.value_ < rhs.value_;
    }
    friend constexpr auto operator<=(Vpn lhs, Vpn rhs) noexcept -> bool {
        return lhs.value_ <= rhs.value_;
    }
    friend constexpr auto operator>(Vpn lhs, Vpn rhs) noexcept -> bool {
        return lhs.value_ > rhs.value_;
    }
    friend constexpr auto operator>=(Vpn lhs, Vpn rhs) noexcept -> bool {
        return lhs.value_ >= rhs.value_;
    }

private:
    uintptr_t value_{};
};

class VPage {
public:
    constexpr VPage() noexcept = default;

    [[nodiscard]] static constexpr auto from_base(VirtAddr address) noexcept
        -> libk::optional<VPage> {
        if (!address.is_aligned(page_size)) {
            return libk::nullopt;
        }
        return VPage{Vpn{address.raw() / page_size}};
    }

    [[nodiscard]] constexpr auto valid() const noexcept -> bool {
        return number_.raw() <= max_page_number_;
    }

    [[nodiscard]] constexpr auto number() const noexcept -> Vpn {
        return number_;
    }

    [[nodiscard]] constexpr auto base() const noexcept -> VirtAddr {
        return VirtAddr{number_.raw() * page_size};
    }

    [[nodiscard]] constexpr auto checked_add(size_t pages) const noexcept
        -> libk::optional<VPage> {
        const auto next = number_.checked_add(pages);
        if (!next.has_value() || next.value().raw() > max_page_number_) {
            return libk::nullopt;
        }
        return VPage{next.value()};
    }

    friend constexpr auto operator==(VPage lhs, VPage rhs) noexcept -> bool {
        return lhs.number_ == rhs.number_;
    }
    friend constexpr auto operator!=(VPage lhs, VPage rhs) noexcept -> bool {
        return !(lhs == rhs);
    }
    friend constexpr auto operator<(VPage lhs, VPage rhs) noexcept -> bool {
        return lhs.number_ < rhs.number_;
    }

private:
    constexpr explicit VPage(Vpn number) noexcept
        : number_(number) {}

    static constexpr uintptr_t max_page_number_ =
        libk::numeric_limits<uintptr_t>::max() / page_size;

    Vpn number_{};
};

class VirtRange {
public:
    constexpr VirtRange() noexcept = default;
    constexpr VirtRange(VirtAddr base, size_t size) noexcept
        : base_(base), size_(size) {}

    [[nodiscard]] static constexpr auto from_bounds(
        VirtAddr base, VirtAddr end) noexcept -> libk::optional<VirtRange> {
        const auto size = base.checked_distance_to(end);
        if (!size.has_value()) {
            return libk::nullopt;
        }
        return VirtRange{base, size.value()};
    }

    [[nodiscard]] constexpr auto valid() const noexcept -> bool {
        return base_.valid() && base_.checked_add(size_).has_value();
    }

    [[nodiscard]] constexpr auto base() const noexcept -> VirtAddr {
        return base_;
    }

    [[nodiscard]] constexpr auto size() const noexcept -> size_t {
        return size_;
    }

    [[nodiscard]] constexpr auto empty() const noexcept -> bool {
        return size_ == 0;
    }

    [[nodiscard]] constexpr auto end() const noexcept -> libk::optional<VirtAddr> {
        return base_.checked_add(size_);
    }

    [[nodiscard]] constexpr auto page_count() const noexcept
        -> libk::optional<size_t> {
        if (!valid() || empty() || !base_.is_aligned(page_size)
            || size_ % page_size != 0) {
            return libk::nullopt;
        }
        return size_ / page_size;
    }

    [[nodiscard]] constexpr auto page_offset(VirtAddr address) const noexcept
        -> libk::optional<size_t> {
        if (!contains(address) || !base_.is_aligned(page_size)
            || !address.is_aligned(page_size)) {
            return libk::nullopt;
        }
        return (address.raw() - base_.raw()) / page_size;
    }

    [[nodiscard]] constexpr auto contains(VirtAddr address) const noexcept -> bool {
        const auto range_end = end();
        return valid() && range_end.has_value()
            && address >= base_ && address < range_end.value();
    }

    [[nodiscard]] constexpr auto contains(VirtRange other) const noexcept -> bool {
        const auto range_end = end();
        const auto other_end = other.end();
        return valid() && other.valid()
            && range_end.has_value() && other_end.has_value()
            && other.base_ >= base_ && other_end.value() <= range_end.value();
    }

    [[nodiscard]] constexpr auto intersects(VirtRange other) const noexcept -> bool {
        const auto range_end = end();
        const auto other_end = other.end();
        return valid() && other.valid() && !empty() && !other.empty()
            && range_end.has_value() && other_end.has_value()
            && base_ < other_end.value() && other.base_ < range_end.value();
    }

    [[nodiscard]] friend constexpr auto operator==(
        VirtRange, VirtRange) noexcept -> bool = default;

private:
    VirtAddr base_{};
    size_t size_{};
};

} // namespace kernel::mm
