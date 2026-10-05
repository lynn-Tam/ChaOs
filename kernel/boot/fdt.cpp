#include <expected>
#include <optional>
#include <boot/fdt.hpp>

#include <libk/assert.hpp>

namespace {

inline constexpr uint32_t FdtMagic = 0xd00dfeed;
inline constexpr uint32_t SupportedVersion = 17;
inline constexpr uint32_t HeaderSize = 10 * sizeof(uint32_t);

inline constexpr uint32_t BeginNodeToken = 0x00000001;
inline constexpr uint32_t EndNodeToken = 0x00000002;
inline constexpr uint32_t PropertyToken = 0x00000003;
inline constexpr uint32_t NopToken = 0x00000004;
inline constexpr uint32_t EndToken = 0x00000009;

[[nodiscard]] auto align_structure(
    libk::ByteReader& reader) noexcept -> bool {

    constexpr size_t Alignment = sizeof(uint32_t);
    const size_t remainder = reader.offset() % Alignment;

    return remainder == 0
        || reader.skip(Alignment - remainder);
}

} // namespace

struct Fdt::Item final {
    enum class Kind : uint8_t {
        BeginNode,
        EndNode,
        Property,
        Nop,
        End,
    };

    Kind kind{};
    uint32_t offset{};
    uint32_t next_offset{};
    libk::StrView name{};
    libk::ByteSpan value{};
};

auto Fdt::open(const void* dtb) noexcept
    -> std::expected<Fdt, FdtError> {

    if (dtb == nullptr) {
        return std::unexpected(FdtError::InvalidHeader);
    }

    const auto* base = static_cast<const uint8_t*>(dtb);

    if (reinterpret_cast<uintptr_t>(base) % sizeof(uint64_t) != 0) {
        return std::unexpected(FdtError::InvalidHeader);
    }

    libk::ByteReader header{base, HeaderSize};

    uint32_t magic{};
    uint32_t total_size{};
    uint32_t structure_offset{};
    uint32_t strings_offset{};
    uint32_t reservations_offset{};
    uint32_t version{};
    uint32_t last_compatible_version{};
    uint32_t boot_cpu_id{};
    uint32_t strings_size{};
    uint32_t structure_size{};

    if (!header.read_be32(magic)
        || !header.read_be32(total_size)
        || !header.read_be32(structure_offset)
        || !header.read_be32(strings_offset)
        || !header.read_be32(reservations_offset)
        || !header.read_be32(version)
        || !header.read_be32(last_compatible_version)
        || !header.read_be32(boot_cpu_id)
        || !header.read_be32(strings_size)
        || !header.read_be32(structure_size)) {
        return std::unexpected(FdtError::InvalidHeader);
    }

    (void)boot_cpu_id;

    auto within = [total_size](uint32_t offset, uint32_t size) {
        return offset >= HeaderSize
            && offset <= total_size
            && size <= total_size - offset;
    };

    if (magic != FdtMagic
        || version < SupportedVersion
        || last_compatible_version > SupportedVersion
        || last_compatible_version > version
        || total_size < HeaderSize
        || reservations_offset % sizeof(uint64_t) != 0
        || structure_offset % sizeof(uint32_t) != 0
        || structure_size < sizeof(uint32_t)
        || !within(structure_offset, structure_size)
        || !within(strings_offset, strings_size)) {
        return std::unexpected(FdtError::InvalidHeader);
    }

    // DTSpec places reservations, structure, then strings after the header.
    if (reservations_offset < HeaderSize
        || structure_offset < reservations_offset
        || structure_offset - reservations_offset
            < 2 * sizeof(uint64_t)
        || strings_offset < structure_offset
        || structure_size > strings_offset - structure_offset) {
        return std::unexpected(FdtError::InvalidHeader);
    }

    Fdt result{
        libk::ByteSpan{base, total_size},
        libk::ByteSpan{base + structure_offset, structure_size},
        libk::ByteSpan{base + strings_offset, strings_size},
        libk::ByteSpan{
            base + reservations_offset,
            structure_offset - reservations_offset,
        },
    };

    if (!result.validate_structure()) {
        return std::unexpected(FdtError::InvalidStructure);
    }

    size_t reservations_size{};
    if (!result.validate_reservations(reservations_size)) {
        return std::unexpected(FdtError::InvalidReservations);
    }

    result.reservations_ = libk::ByteSpan{
        result.reservations_.data(),
        reservations_size,
    };

    return (result);
}

auto Fdt::string_at(
    uint32_t offset,
    libk::StrView& string) const noexcept -> bool {

    if (offset >= strings_.size()) {
        return false;
    }

    libk::ByteReader reader{
        strings_.data() + offset,
        strings_.size() - offset,
    };

    return reader.read_cstr(string);
}

auto Fdt::read_item(
    uint32_t offset,
    Item& item) const noexcept -> bool {

    if (offset >= structure_.size()
        || offset % sizeof(uint32_t) != 0) {
        return false;
    }

    libk::ByteReader reader{
        structure_.data(),
        structure_.size(),
    };

    if (!reader.skip(offset)) {
        return false;
    }

    uint32_t token{};
    if (!reader.read_be32(token)) {
        return false;
    }

    item = {};
    item.offset = offset;

    switch (token) {
    case BeginNodeToken:
        item.kind = Item::Kind::BeginNode;
        if (!reader.read_cstr(item.name)
            || !align_structure(reader)) {
            return false;
        }
        break;

    case EndNodeToken:
        item.kind = Item::Kind::EndNode;
        break;

    case PropertyToken: {
        item.kind = Item::Kind::Property;

        uint32_t length{};
        uint32_t name_offset{};

        if (!reader.read_be32(length)
            || !reader.read_be32(name_offset)
            || !string_at(name_offset, item.name)
            || !reader.take_bytes(length, item.value)
            || !align_structure(reader)) {
            return false;
        }
        break;
    }

    case NopToken:
        item.kind = Item::Kind::Nop;
        break;

    case EndToken:
        item.kind = Item::Kind::End;
        break;

    default:
        return false;
    }

    item.next_offset =
        static_cast<uint32_t>(reader.offset());

    return true;
}

auto Fdt::validate_structure() noexcept -> bool {
    uint32_t offset = 0;
    int depth = -1;
    bool saw_root = false;

    for (;;) {
        Item item{};
        if (!read_item(offset, item)) {
            return false;
        }

        switch (item.kind) {
        case Item::Kind::BeginNode:
            if (depth == -1) {
                if (saw_root || !item.name.empty()) {
                    return false;
                }
                root_offset_ = item.offset;
                saw_root = true;
            }
            ++depth;
            break;

        case Item::Kind::EndNode:
            if (depth < 0) {
                return false;
            }
            --depth;
            break;

        case Item::Kind::Property:
            if (depth < 0) {
                return false;
            }
            break;

        case Item::Kind::Nop:
            break;

        case Item::Kind::End:
            return saw_root
                && depth == -1
                && item.next_offset == structure_.size();
        }

        offset = item.next_offset;
    }
}

auto Fdt::validate_reservations(
    size_t& actual_size) const noexcept -> bool {

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
            actual_size = reader.offset();
            return true;
        }
    }
}

auto Fdt::node_name(FdtNode node) const noexcept
    -> libk::StrView {

    Item item{};
    const bool valid =
        read_item(node.offset_, item)
        && item.kind == Item::Kind::BeginNode;

    libk_assert(valid);
    return item.name;
}

auto Fdt::property(
    FdtNode node,
    libk::StrView name) const noexcept
    -> std::optional<libk::ByteSpan> {

    Item begin{};
    if (!read_item(node.offset_, begin)
        || begin.kind != Item::Kind::BeginNode) {
        return std::nullopt;
    }

    uint32_t offset = begin.next_offset;
    int depth = 0;

    for (;;) {
        Item item{};
        if (!read_item(offset, item)) {
            return std::nullopt;
        }

        switch (item.kind) {
        case Item::Kind::BeginNode:
            ++depth;
            break;

        case Item::Kind::EndNode:
            if (depth == 0) {
                return std::nullopt;
            }
            --depth;
            break;

        case Item::Kind::Property:
            if (depth == 0 && item.name == name) {
                return item.value;
            }
            break;

        case Item::Kind::Nop:
            break;

        case Item::Kind::End:
            return std::nullopt;
        }

        offset = item.next_offset;
    }
}

auto Fdt::property_count(
    FdtNode node,
    libk::StrView name) const noexcept -> size_t {

    Item begin{};
    if (!read_item(node.offset_, begin)
        || begin.kind != Item::Kind::BeginNode) {
        return 0;
    }

    uint32_t offset = begin.next_offset;
    int depth = 0;
    size_t count = 0;
    for (;;) {
        Item item{};
        if (!read_item(offset, item)) {
            return count;
        }
        switch (item.kind) {
        case Item::Kind::BeginNode:
            ++depth;
            break;
        case Item::Kind::EndNode:
            if (depth == 0) {
                return count;
            }
            --depth;
            break;
        case Item::Kind::Property:
            if (depth == 0 && item.name == name) {
                ++count;
            }
            break;
        case Item::Kind::End:
            return count;
        case Item::Kind::Nop:
            break;
        }
        offset = item.next_offset;
    }
}

auto Fdt::first_child(FdtNode parent) const noexcept
    -> std::optional<FdtNode> {

    Item begin{};
    if (!read_item(parent.offset_, begin)
        || begin.kind != Item::Kind::BeginNode) {
        return std::nullopt;
    }

    uint32_t offset = begin.next_offset;

    for (;;) {
        Item item{};
        if (!read_item(offset, item)) {
            return std::nullopt;
        }

        switch (item.kind) {
        case Item::Kind::BeginNode:
            return FdtNode{item.offset};

        case Item::Kind::EndNode:
        case Item::Kind::End:
            return std::nullopt;

        case Item::Kind::Property:
        case Item::Kind::Nop:
            offset = item.next_offset;
            break;
        }
    }
}

auto Fdt::next_sibling(FdtNode node) const noexcept
    -> std::optional<FdtNode> {

    Item begin{};
    if (!read_item(node.offset_, begin)
        || begin.kind != Item::Kind::BeginNode) {
        return std::nullopt;
    }

    uint32_t offset = begin.next_offset;
    int depth = 0;

    // Skip the current node's full subtree first.
    for (;;) {
        Item item{};
        if (!read_item(offset, item)) {
            return std::nullopt;
        }

        if (item.kind == Item::Kind::BeginNode) {
            ++depth;
        } else if (item.kind == Item::Kind::EndNode) {
            if (depth == 0) {
                offset = item.next_offset;
                break;
            }
            --depth;
        } else if (item.kind == Item::Kind::End) {
            return std::nullopt;
        }

        offset = item.next_offset;
    }

    // After the current node, the next BEGIN_NODE is its sibling.
    // END_NODE means the parent has ended.
    for (;;) {
        Item item{};
        if (!read_item(offset, item)) {
            return std::nullopt;
        }

        switch (item.kind) {
        case Item::Kind::BeginNode:
            return FdtNode{item.offset};

        case Item::Kind::EndNode:
        case Item::Kind::End:
            return std::nullopt;

        case Item::Kind::Property:
        case Item::Kind::Nop:
            offset = item.next_offset;
            break;
        }
    }
}

auto Fdt::child(
    FdtNode parent,
    libk::StrView name) const noexcept
    -> std::optional<FdtNode> {

    for (auto node = first_child(parent);
         node;
         node = next_sibling(*node)) {

        if (node_name(*node) == name) {
            return node;
        }
    }

    return std::nullopt;
}

auto Fdt::read_u32(
    libk::ByteSpan bytes,
    uint32_t& value) noexcept -> bool {

    if (bytes.size() != sizeof(uint32_t)) {
        return false;
    }

    libk::ByteReader reader{
        bytes.data(),
        bytes.size(),
    };

    return reader.read_be32(value);
}

auto Fdt::first_string(
    libk::ByteSpan bytes) noexcept
    -> std::optional<libk::StrView> {

    libk::ByteReader reader{
        bytes.data(),
        bytes.size(),
    };

    libk::StrView string{};

    if (!reader.read_cstr(string)) {
        return std::nullopt;
    }

    return string;
}

