#pragma once

#include <stddef.h>
#include <stdint.h>

#include <libk/byte_reader.hpp>
#include <libk/expected.hpp>
#include <libk/optional.hpp>
#include <libk/span.hpp>
#include <libk/string_view.hpp>

namespace kernel::boot {

class FdtNode final {
private:
    explicit constexpr FdtNode(uint32_t offset) noexcept
        : offset_(offset) {}

    uint32_t offset_{};

    friend class Fdt;
};

enum class FdtError : uint8_t {
    InvalidHeader,
    InvalidStructure,
    InvalidReservations,
};

class Fdt final {
public:
    [[nodiscard]] static auto open(const void* dtb) noexcept
        -> libk::Expected<Fdt, FdtError>;

    [[nodiscard]] auto size() const noexcept -> size_t {
        return blob_.size();
    }

    [[nodiscard]] auto root() const noexcept -> FdtNode {
        return FdtNode{root_offset_};
    }

    [[nodiscard]] auto node_name(FdtNode node) const noexcept
        -> libk::StrView;

    [[nodiscard]] auto property(
        FdtNode node,
        libk::StrView name) const noexcept
        -> libk::optional<libk::ByteSpan>;

    [[nodiscard]] auto property_count(
        FdtNode node,
        libk::StrView name) const noexcept -> size_t;

    [[nodiscard]] auto first_child(FdtNode parent) const noexcept
        -> libk::optional<FdtNode>;

    [[nodiscard]] auto next_sibling(FdtNode node) const noexcept
        -> libk::optional<FdtNode>;

    [[nodiscard]] auto child(
        FdtNode parent,
        libk::StrView name) const noexcept
        -> libk::optional<FdtNode>;

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
        -> libk::optional<libk::StrView>;

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

} // namespace kernel::boot
