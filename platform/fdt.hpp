#pragma once
#include <boot/info.hpp>
#include <ranges>
#include <array>

#include <stddef.h>
#include <stdint.h>

#include <libk/byte_reader.hpp>
#include <expected>
#include <optional>
#include <libk/span.hpp>
#include <libk/string_view.hpp>

enum class FdtError : uint8_t {
    InvalidHeader,
    InvalidStructure,
    InvalidReservations,
};

class Fdt final {
public:
    [[nodiscard]] static auto open(const void* dtb) noexcept
        -> std::expected<Fdt, FdtError>;

    [[nodiscard]] auto size() const noexcept -> size_t {
        return blob_.size();
    }

    auto parent(u32) const noexcept -> std::optional<u32>;
    auto path(libk::StrView) const noexcept -> std::optional<u32>;
    auto phandle(u32) const noexcept -> std::optional<u32>;
    auto find(libk::StrView) const noexcept -> std::optional<u32>;
    auto number(u32, libk::StrView) const noexcept -> std::optional<u32>;
    auto matches(u32, libk::StrView) const noexcept -> bool;

    [[nodiscard]] auto root() const noexcept -> u32 {
        return root_offset_;
    }

    [[nodiscard]] auto node_name(u32 node) const noexcept
        -> libk::StrView;

    [[nodiscard]] auto property(
        u32 node,
        libk::StrView name) const noexcept
        -> std::optional<libk::ByteSpan>;

    [[nodiscard]] auto first_child(u32 parent) const noexcept
        -> std::optional<u32>;

    [[nodiscard]] auto next_sibling(u32 node) const noexcept
        -> std::optional<u32>;

    [[nodiscard]] auto child(
        u32 parent,
        libk::StrView name) const noexcept
        -> std::optional<u32>;

    template<typename Visitor>
    [[nodiscard]] auto for_each_reservation(
        Visitor&& visitor) const noexcept -> bool {

        libk::ByteReader reader{
            reservations_.data(),
            reservations_.size(),
        };

        for (;;) {
            uint64_t address{};
            uint64_t size{};

            if (!reader.read_be64(address)
                || !reader.read_be64(size)) {
                return false;
            }

            if (address == 0 && size == 0) {
                return true;
            }

            if (!visitor(address, size)) {
                return false;
            }
        }
    }

    [[nodiscard]] static auto read_u32(
        libk::ByteSpan bytes,
        uint32_t& value) noexcept -> bool;

    [[nodiscard]] static auto first_string(
        libk::ByteSpan bytes) noexcept
        -> std::optional<libk::StrView>;

private:
    struct Item;

    Fdt(
        libk::ByteSpan blob,
        libk::ByteSpan structure,
        libk::ByteSpan strings,
        libk::ByteSpan reservations) noexcept
        : blob_(blob),
          structure_(structure),
          strings_(strings),
          reservations_(reservations) {}

    [[nodiscard]] auto read_item(
        uint32_t offset,
        Item& item) const noexcept -> bool;

    [[nodiscard]] auto string_at(
        uint32_t offset,
        libk::StrView& string) const noexcept -> bool;

    [[nodiscard]] auto validate_structure() noexcept -> bool;

    [[nodiscard]] auto validate_reservations(
        size_t& actual_size) const noexcept -> bool;

    libk::ByteSpan blob_{};
    libk::ByteSpan structure_{};
    libk::ByteSpan strings_{};
    libk::ByteSpan reservations_{};

    uint32_t root_offset_{};
};


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

// Immutable runtime wiring. Table bytes borrow the retained firmware Mem.
struct FwRange {
    usize pa{}, size{};
    auto pages() const noexcept { return mm::Pages::covering_bytes(mm::Phys{pa}, size); }
};
struct FwPci {
    FwRange cfg, window;
    libk::ByteSpan routes{}, devices{};
    std::array<u32, 4> mask{};
    u32 iommu{};
    auto irq(u16 rid, u8 pin) const noexcept -> std::optional<u32>;
    auto device(u16 rid) const noexcept -> std::optional<u16>;
};
struct FwHw {
    FwRange plic, uart, iommu;
    usize ctx{};
    u32 nirq{}, uart_irq{}, iommu_irq{};
    FwPci pci{};
};
auto read_fdt(BootInfo&, mm::RegionList&, CpuHwId, mm::Phys,
              const void*, FwHw&) noexcept
    -> std::expected<void, FdtError>;
