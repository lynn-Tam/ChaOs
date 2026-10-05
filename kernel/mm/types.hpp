#pragma once
#include <base/types.hpp>
#include <compare>
#include <concepts>
#include <optional>
#include <limits>
#include <ranges>
#include <libk/bits.hpp>
#include <libk/key.hpp>

namespace mm {
inline constexpr usize page_size = 4096;
enum class Space { Phys, Virt };

template <Space space, size_t unit = 1> class Addr {
    uintptr_t value_{};

  public:
    static constexpr size_t stride = unit;
    using difference_type = ptrdiff_t;
    constexpr auto operator++() noexcept -> Addr& {
        ++value_;
        return *this;
    }
    constexpr auto operator++(int) noexcept -> Addr {
        auto old = *this;
        ++*this;
        return old;
    }
    constexpr Addr() noexcept = default;
    constexpr explicit Addr(uintptr_t value) noexcept : value_(value) {}
    constexpr auto raw() const noexcept -> uintptr_t { return value_; }
    constexpr auto operator<=>(const Addr&) const noexcept = default;

    constexpr auto valid() const noexcept -> bool {
        if constexpr (unit != 1)
            return value_ <= max();
        else
            return space == Space::Phys || value_ != 0;
    }
    static constexpr auto null() noexcept -> Addr
        requires(unit == 1)
    {
        return {};
    }
    constexpr auto is_null() const noexcept -> bool
        requires(unit == 1)
    {
        return value_ == 0;
    }
    static constexpr auto max() noexcept -> uintptr_t {
        return std::numeric_limits<uintptr_t>::max() / unit;
    }
    constexpr auto checked_add(size_t n) const noexcept -> std::optional<Addr> {
        return value_ <= max() && n <= max() - value_ ? std::optional<Addr>{Addr{value_ + n}}
                                                      : std::nullopt;
    }
    constexpr auto checked_sub(size_t n) const noexcept -> std::optional<Addr> {
        return n <= value_ ? std::optional<Addr>{Addr{value_ - n}} : std::nullopt;
    }
    constexpr auto checked_distance_to(Addr end) const noexcept -> std::optional<size_t> {
        return end >= *this ? std::optional<size_t>{end.raw() - value_} : std::nullopt;
    }
    constexpr auto is_aligned(size_t n) const noexcept -> bool
        requires(unit == 1)
    {
        return n && value_ % n == 0;
    }
    constexpr auto aligned_down(size_t n) const noexcept -> std::optional<Addr>
        requires(unit == 1)
    {
        return n ? std::optional<Addr>{Addr{value_ - value_ % n}} : std::nullopt;
    }
    constexpr auto aligned_up(size_t n) const noexcept -> std::optional<Addr>
        requires(unit == 1)
    {
        if (!n)
            return std::nullopt;
        return value_ % n ? checked_add(n - value_ % n) : std::optional<Addr>{*this};
    }
    static constexpr auto from_base(Addr<space> a) noexcept -> std::optional<Addr>
        requires(unit != 1)
    {
        return a.is_aligned(unit) ? std::optional<Addr>{Addr{a.raw() / unit}} : std::nullopt;
    }
    constexpr auto base() const noexcept -> Addr<space>
        requires(unit != 1)
    {
        return Addr<space>{value_ * unit};
    }
};

using Phys = Addr<Space::Phys>;
using Virt = Addr<Space::Virt>;
using Page = Addr<Space::Phys, page_size>;
using VPage = Addr<Space::Virt, page_size>;

template <class T> class Range {
    T base_{};
    usize size_{};
    static constexpr bool scalar = std::integral<T>;
    static constexpr usize stride() noexcept {
        if constexpr (scalar)
            return 1;
        else
            return T::stride;
    }
    static constexpr usize raw(T v) noexcept {
        if constexpr (scalar)
            return v;
        else
            return v.raw();
    }

  public:
    constexpr Range() noexcept = default;
    constexpr Range(T base, usize size) noexcept : base_(base), size_(size) {}
    constexpr auto base() const noexcept -> T { return base_; }
    constexpr auto size() const noexcept -> usize { return size_; }
    constexpr bool empty() const noexcept { return size_ == 0; }
    constexpr auto limit() const noexcept -> std::optional<T> {
        return size_ <= std::numeric_limits<usize>::max() - raw(base_)
                   ? std::optional<T>{T{raw(base_) + size_}}
                   : std::nullopt;
    }
    constexpr bool valid() const noexcept {
        if constexpr (scalar)
            return size_ && limit().has_value();
        else if constexpr (stride() == 1)
            return base_.valid() && limit().has_value();
        else
            return base_.valid() && size_ && size_ <= T::max() &&
                   size_ - 1 <= T::max() - raw(base_);
    }
    constexpr bool within(usize n) const noexcept
        requires scalar
    {
        return valid() && *limit() <= n;
    }
    constexpr bool contains(T p) const noexcept { return valid() && p >= base_ && p < *limit(); }
    constexpr bool contains(Range r) const noexcept {
        return valid() && r.valid() && r.base_ >= base_ && *r.limit() <= *limit();
    }
    constexpr bool intersects(Range r) const noexcept {
        return valid() && r.valid() && !empty() && !r.empty() && base_ < *r.limit() &&
               r.base_ < *limit();
    }
    constexpr auto begin() const noexcept { return std::views::iota(base_, *limit()).begin(); }
    constexpr auto end() const noexcept { return std::views::iota(base_, *limit()).end(); }
    constexpr auto page_count() const noexcept {
        if constexpr (scalar || stride() == page_size)
            return size_;
        else
            return valid() && !empty() && base_.is_aligned(page_size) && size_ % page_size == 0
                       ? std::optional<usize>{size_ / page_size}
                       : std::nullopt;
    }
    constexpr auto byte_size() const noexcept -> usize
        requires(!scalar && stride() == page_size)
    {
        return size_ * page_size;
    }
    constexpr auto page_offset(T p) const noexcept -> std::optional<usize>
        requires(!scalar && stride() == 1)
    {
        return contains(p) && base_.is_aligned(page_size) && p.is_aligned(page_size)
                   ? std::optional<usize>{(raw(p) - raw(base_)) / page_size}
                   : std::nullopt;
    }
    static constexpr auto from_bounds(T base, T end) noexcept -> std::optional<Range> {
        return end >= base ? std::optional<Range>{Range{base, raw(end) - raw(base)}} : std::nullopt;
    }
    static constexpr auto from_aligned_bytes(Phys base, usize bytes) noexcept
        -> std::optional<Range>
        requires std::same_as<T, Page>
    {
        auto p = Page::from_base(base);
        Range r{p.value_or(Page{}), bytes / page_size};
        return p && bytes % page_size == 0 && r.valid() ? std::optional<Range>{r} : std::nullopt;
    }
    static constexpr auto covering_bytes(Phys base, usize bytes) noexcept -> std::optional<Range>
        requires std::same_as<T, Page>
    {
        auto end = base.checked_add(bytes);
        auto lo = base.aligned_down(page_size),
             hi = end ? end->aligned_up(page_size) : std::nullopt;
        return bytes && lo && hi ? from_aligned_bytes(*lo, hi->raw() - lo->raw()) : std::nullopt;
    }
    static constexpr auto contained_bytes(Phys base, usize bytes) noexcept -> std::optional<Range>
        requires std::same_as<T, Page>
    {
        auto end = base.checked_add(bytes);
        auto lo = base.aligned_up(page_size),
             hi = end ? end->aligned_down(page_size) : std::nullopt;
        return bytes && lo && hi && *hi > *lo ? from_aligned_bytes(*lo, hi->raw() - lo->raw())
                                              : std::nullopt;
    }
    friend constexpr auto operator==(Range, Range) noexcept -> bool = default;
};

using Pages = Range<Page>;
using VRange = Range<Virt>;
using ObjectRange = Range<usize>;
class Map;
using MapId = libk::key<Map>;

// CPU mapping request. Native preserves the platform PMA, for RAM or devices.
enum class CpuAttr : u8 { Native, Nc, Io };

enum class MemoryType : u8 {
    Normal,
    Uncached,
    Device,
};

[[nodiscard]] constexpr auto type_bit(MemoryType type) noexcept -> u8 {
    return static_cast<u8>(u8{1} << std::to_underlying(type));
}

using MemoryTypes = libk::enum_flags<MemoryType, type_bit>;

[[nodiscard]] constexpr auto valid_memory_types(MemoryTypes types) noexcept -> bool {
    constexpr u8 valid =
        MemoryTypes::of(MemoryType::Normal, MemoryType::Uncached, MemoryType::Device).raw();
    return !types.empty() && (types.raw() & ~valid) == 0;
}

enum class Perm : u8 {
    Read = u8{1} << 0,
    Write = u8{1} << 1,
    Execute = u8{1} << 2,
};

using Perms = libk::enum_flags<Perm>;

[[nodiscard]] constexpr auto valid_perms(Perms access) noexcept -> bool {
    constexpr u8 valid = Perms::of(Perm::Read, Perm::Write, Perm::Execute).raw();
    return !access.empty() && (access.raw() & ~valid) == 0 &&
           (!access.contains(Perm::Write) || access.contains(Perm::Read));
}

} // namespace mm
