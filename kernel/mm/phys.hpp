#pragma once

#include <base/types.hpp>
#include <expected>
#include <libk/checked_arithmetic.hpp>
#include <libk/inplace_vector.hpp>
#include <mm/types.hpp>
#include <ranges>
#include <span>

namespace mm {

struct Region {
    enum class Kind : u8 { Ram, Boot, Kernel, Firmware, Mmio };
    Pages range{};
    Kind kind{Kind::Mmio};
    CpuAttr attr{CpuAttr::Native};
    constexpr bool valid() const noexcept { return range.valid() && attr <= CpuAttr::Io; }
    constexpr bool is_ram() const noexcept { return kind != Kind::Mmio; }
    constexpr bool is_reclaimable() const noexcept { return kind == Kind::Boot; }
};

inline constexpr usize max_regions = 32;
using RegionList = libk::InplaceVector<Region, max_regions>;

// Borrowed projections: classification remains in the owner's one region list.
inline auto ram(std::span<const Region> regions) noexcept {
    return regions | std::views::filter([](const Region& r) { return r.is_ram(); }) |
           std::views::transform([](const Region& r) { return r.range; });
}

enum class PhysErr : u8 { Invalid, NoRam, Overlap, Capacity };

// Firmware input may overlap; finish sweeps boundaries without another index.
class PhysMap {
  public:
    auto add_ram(Pages r, CpuAttr attr = CpuAttr::Native) noexcept -> std::expected<void, PhysErr>;
    auto reserve(Pages r, Region::Kind kind) noexcept -> std::expected<void, PhysErr>;
    auto ram() const noexcept {
        return input_.span() |
               std::views::filter([](const Region& r) { return r.kind == Region::Kind::Ram; }) |
               std::views::transform([](const Region& r) { return r.range; });
    }
    auto finish(RegionList& out) && noexcept -> std::expected<void, PhysErr>;

  private:
    RegionList input_{};
};

// No independent owner, copied ranges, or second initialization protocol.
// The list must stay sorted, disjoint and alive for the lifetime of the view.
class DirectMap {
  public:
    struct Layout {
        Phys physical_base{};
        Virt virtual_base{};
        usize window_size{};
    };
    enum class Error : u8 { Invalid, OutsideWindow, NotMapped, Overflow };
    DirectMap() noexcept = default;
    DirectMap(std::span<const Region> regions, Layout layout) noexcept : regions_(regions), layout_(layout) {}
    bool valid() const noexcept;
    explicit operator bool() const noexcept { return !regions_.empty(); }
    auto ranges() const noexcept { return mm::ram(regions_); }
    auto map(Phys address, usize size) const noexcept -> std::expected<Virt, Error>;
    auto unmap(Virt address, usize size) const noexcept -> std::expected<Phys, Error>;
    template <class T> auto ptr(Phys address, usize count = 1) const noexcept -> std::expected<T*, Error> {
        auto bytes = libk::checked_multiply(sizeof(T), count);
        if (!bytes || !*bytes) return std::unexpected(Error::Overflow);
        auto va = map(address, *bytes);
        if (!va) return std::unexpected(va.error());
        return reinterpret_cast<T*>(va->raw());
    }
    Phys physical_base() const noexcept { return layout_.physical_base; }
    Virt virtual_base() const noexcept { return layout_.virtual_base; }
    usize window_size() const noexcept { return layout_.window_size; }

  private:
    bool contains(Pages r) const noexcept;
    std::span<const Region> regions_{};
    Layout layout_{};
};

} // namespace mm
