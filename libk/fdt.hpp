#pragma once
#include <array>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <libk/byte_reader.hpp>

namespace libk {

// Immutable firmware view. Node ids are structure offsets, never authorities.
// The backing bytes must remain readable and unchanged for the view's lifetime.
class fdt {
    using bytes = std::span<const uint8_t>;
    static constexpr uint32_t header_size = 40;
    enum token : uint32_t { begin = 1, end_node = 2, prop = 3, nop = 4, end = 9 };
    struct item {
        uint32_t kind{}, offset{}, next{};
        std::string_view name{};
        bytes value{};
    };
    bytes blob_, structure_, strings_, reservations_;
    uint32_t root_{};
    fdt(bytes blob, bytes structure, bytes strings, bytes reservations, uint32_t root) noexcept
        : blob_(blob), structure_(structure), strings_(strings), reservations_(reservations), root_(root) {}

    static auto align(byte_reader& r) noexcept -> bool {
        return r.skip((4 - r.offset() % 4) % 4);
    }
    auto read(uint32_t offset) const noexcept -> std::optional<item> {
        if (offset % 4 || offset >= structure_.size()) return {};
        byte_reader r{structure_.data(), structure_.size()};
        item i{.offset = offset};
        if (!r.skip(offset) || !r.read_be(i.kind)) return {};
        switch (i.kind) {
        case begin:
            if (!r.read_cstr(i.name) || !align(r)) return {};
            break;
        case prop: {
            uint32_t size, name;
            if (!r.read_be(size) || !r.read_be(name) || name >= strings_.size()) return {};
            byte_reader s{strings_.data() + name, strings_.size() - name};
            if (!s.read_cstr(i.name) || !r.take_bytes(size, i.value) || !align(r)) return {};
            break;
        }
        case end_node: case nop: case end: break;
        default: return {};
        }
        i.next = static_cast<uint32_t>(r.offset());
        return i;
    }
    // Both child and sibling traversal skip tokens until a begin at depth zero.
    auto scan(uint32_t off, int depth) const noexcept -> std::optional<uint32_t> {
        for (auto i = read(off); i; i = read(i->next)) {
            if (i->kind == begin && depth++ == 0) return i->offset;
            if (i->kind == end_node && --depth < 0) break;
            if (i->kind == end) break;
        }
        return {};
    }

public:
    enum class error { invalid_header, invalid_structure, invalid_reservations };
    static auto open(bytes blob) noexcept -> std::expected<fdt, error> {
        if (blob.size() < header_size) return std::unexpected(error::invalid_header);
        byte_reader r{blob.data(), header_size};
        std::array<uint32_t, 10> h;
        for (auto& n : h) if (!r.read_be(n)) return std::unexpected(error::invalid_header);
        auto within = [&](uint32_t off, uint32_t size) {
            return off >= header_size && off <= h[1] && size <= h[1] - off;
        };
        if (h[0] != 0xd00dfeed || h[1] < header_size || h[1] > blob.size() ||
            h[5] < 17 || h[6] > 17 || h[6] > h[5] || h[4] % 8 || h[2] % 4 ||
            h[9] < 4 || !within(h[2], h[9]) || !within(h[3], h[8]) ||
            h[4] < header_size || h[2] < h[4] || h[2] - h[4] < 16 ||
            h[3] < h[2] || h[9] > h[3] - h[2])
            return std::unexpected(error::invalid_header);
        fdt t{blob.first(h[1]), blob.subspan(h[2], h[9]),
              blob.subspan(h[3], h[8]), blob.subspan(h[4], h[2] - h[4]), 0};
        int depth = -1;
        bool root = false;
        for (auto i = t.read(0);; i = t.read(i->next)) {
            if (!i) return std::unexpected(error::invalid_structure);
            switch (i->kind) {
            case begin:
                if (depth == -1) {
                    if (root || !i->name.empty()) return std::unexpected(error::invalid_structure);
                    root = true;
                    t.root_ = i->offset;
                }
                ++depth;
                break;
            case end_node: if (depth-- < 0) return std::unexpected(error::invalid_structure); break;
            case prop: if (depth < 0) return std::unexpected(error::invalid_structure); break;
            default: break;
            }
            if (i->kind == end) {
                if (!root || depth != -1 || i->next != t.structure_.size())
                    return std::unexpected(error::invalid_structure);
                break;
            }
        }
        if (!t.for_each_reservation([](uint64_t, uint64_t) { return true; }))
            return std::unexpected(error::invalid_reservations);
        return t;
    }
    // Firmware entry supplies a trusted, mapped blob before its length is known.
    // Consumers of bounded mappings use the span overload instead.
    static auto open(const void* pointer) noexcept -> std::expected<fdt, error> {
        if (!pointer) return std::unexpected(error::invalid_header);
        auto* p = static_cast<const uint8_t*>(pointer);
        byte_reader r{p + 4, 4};
        uint32_t size;
        if (!r.read_be(size)) return std::unexpected(error::invalid_header);
        return open(bytes{p, size});
    }
    auto size() const noexcept -> size_t { return blob_.size(); }
    auto root() const noexcept -> uint32_t { return root_; }
    auto node_name(uint32_t node) const noexcept -> std::string_view {
        const auto i = read(node);
        return i && i->kind == begin ? i->name : std::string_view{};
    }
    auto property(uint32_t node, std::string_view name) const noexcept -> std::optional<bytes> {
        auto i = read(node);
        if (!i || i->kind != begin) return {};
        std::optional<bytes> value;
        int depth = 0;
        for (i = read(i->next); i; i = read(i->next)) {
            if (i->kind == begin) ++depth;
            if (i->kind == end_node && depth-- == 0) return value;
            if (i->kind == prop && !depth && i->name == name) {
                if (value) return {}; // Ambiguous duplicate property.
                value = i->value;
            }
        }
        return {};
    }
    auto first_child(uint32_t node) const noexcept -> std::optional<uint32_t> {
        const auto i = read(node);
        return i && i->kind == begin ? scan(i->next, 0) : std::nullopt;
    }
    auto next_sibling(uint32_t node) const noexcept -> std::optional<uint32_t> {
        const auto i = read(node);
        return i && i->kind == begin ? scan(i->next, 1) : std::nullopt;
    }
    auto child(uint32_t node, std::string_view name) const noexcept -> std::optional<uint32_t> {
        for (auto n = first_child(node); n; n = next_sibling(*n))
            if (node_name(*n) == name) return n;
        return {};
    }
    auto parent(uint32_t node) const noexcept -> std::optional<uint32_t> {
        // No depth limit or secondary index: two bounded passes over the blob.
        int depth = 0, target = -1;
        for (auto i = read(root_); i && i->offset <= node; i = read(i->next)) {
            if (i->kind == begin) {
                if (i->offset == node) { target = depth - 1; break; }
                ++depth;
            } else if (i->kind == end_node) --depth;
        }
        if (target < 0) return {};
        std::optional<uint32_t> result;
        depth = 0;
        for (auto i = read(root_); i && i->offset < node; i = read(i->next)) {
            if (i->kind == begin) {
                if (depth++ == target) result = i->offset;
            } else if (i->kind == end_node) --depth;
        }
        return result;
    }
    static auto read_u32(bytes p, uint32_t& n) noexcept -> bool {
        byte_reader r{p.data(), p.size()};
        return p.size() == 4 && r.read_be(n);
    }
    static auto first_string(bytes p) noexcept -> std::optional<std::string_view> {
        byte_reader r{p.data(), p.size()};
        std::string_view s;
        return r.read_cstr(s) ? std::optional{s} : std::nullopt;
    }
    auto number(uint32_t node, std::string_view name) const noexcept -> std::optional<uint32_t> {
        const auto p = property(node, name);
        uint32_t n;
        return p && read_u32(*p, n) ? std::optional{n} : std::nullopt;
    }
    static auto read_cells(byte_reader& r, uint32_t count, uint64_t& value) noexcept -> bool {
        if (count > 2) return false;
        value = 0;
        while (count--) {
            uint32_t word;
            if (!r.read_be(word)) return false;
            value = (value << 32) | word;
        }
        return true;
    }
    auto cell_count(uint32_t node, std::string_view key, uint32_t fallback) const noexcept -> uint32_t {
        return property(node, key) ? number(node, key).value_or(UINT32_MAX) : fallback;
    }
    struct range { uint64_t address{}, size{}; };
    auto translate(uint32_t bus, range span) const noexcept -> std::optional<range> {
        while (const auto up = parent(bus)) {
            const auto ranges = property(bus, "ranges");
            if (!ranges) return {};
            if (!ranges->empty()) {
                byte_reader r{ranges->data(), ranges->size()};
                bool found{};
                while (r.remaining()) {
                    uint64_t child, physical, size;
                    if (!read_cells(r, cell_count(bus, "#address-cells", 2), child) ||
                        !read_cells(r, cell_count(*up, "#address-cells", 2), physical) ||
                        !read_cells(r, cell_count(bus, "#size-cells", 1), size)) return {};
                    if (span.address < child || span.address - child > size ||
                        span.size > size - (span.address - child)) continue;
                    if (physical > UINT64_MAX - (span.address - child)) return {};
                    span.address = physical + span.address - child;
                    found = true;
                    break;
                }
                if (!found) return {};
            }
            bus = *up;
        }
        if (!span.size || span.address > UINT64_MAX - span.size) return {};
        return span;
    }
    auto reg(uint32_t node, size_t index = 0) const noexcept -> std::optional<range> {
        const auto up = parent(node);
        const auto regs = property(node, "reg");
        if (!up || !regs) return {};
        const auto ac = cell_count(*up, "#address-cells", 2), sc = cell_count(*up, "#size-cells", 1);
        if (!ac || ac > 2 || !sc || sc > 2) return {};
        const auto stride = size_t{ac + sc} * 4;
        if (regs->size() % stride || index >= regs->size() / stride) return {};
        byte_reader r{regs->data() + index * stride, stride};
        range result;
        if (!read_cells(r, ac, result.address) || !read_cells(r, sc, result.size)) return {};
        return translate(*up, result);
    }
    auto matches(uint32_t node, std::string_view compatible) const noexcept -> bool {
        const auto p = property(node, "compatible");
        if (!p) return false;
        byte_reader r{p->data(), p->size()};
        std::string_view s;
        while (r.remaining() && r.read_cstr(s)) if (s == compatible) return true;
        return false;
    }
    auto find(std::string_view compatible) const noexcept -> std::optional<uint32_t> {
        std::optional<uint32_t> found;
        for (auto i = read(root_); i; i = read(i->next)) {
            if (i->kind != begin || !matches(i->offset, compatible)) continue;
            bool enabled = true;
            for (auto n = std::optional{i->offset}; n; n = parent(*n)) {
                const auto p = property(*n, "status");
                const auto s = p ? first_string(*p) : std::nullopt;
                if (p && s != "okay" && s != "ok") enabled = false;
            }
            if (!enabled) continue;
            if (found) return {}; // This connector requires one enabled instance.
            found = i->offset;
        }
        return found;
    }
    auto phandle(uint32_t value) const noexcept -> std::optional<uint32_t> {
        if (!value || value == UINT32_MAX) return {};
        for (auto i = read(root_); i; i = read(i->next))
            if (i->kind == begin && number(i->offset, "phandle") == value) return i->offset;
        return {};
    }
    auto path(std::string_view path) const noexcept -> std::optional<uint32_t> {
        path = path.substr(0, path.find(':'));
        if (path.empty()) return {};
        if (path.front() != '/') {
            const auto aliases = child(root_, "aliases");
            const auto p = aliases ? property(*aliases, path) : std::nullopt;
            const auto s = p ? first_string(*p) : std::nullopt;
            if (!s || s->empty() || s->front() != '/') return {};
            path = *s;
        }
        uint32_t node = root_;
        for (size_t pos = 1; pos < path.size();) {
            const auto slash = path.find('/', pos);
            const auto next = child(node, path.substr(pos, slash - pos));
            if (!next) return {};
            node = *next;
            if (slash == path.npos) break;
            pos = slash + 1;
        }
        return node;
    }
    template<class F> auto for_each_reservation(F&& visit) const noexcept -> bool {
        byte_reader r{reservations_.data(), reservations_.size()};
        for (;;) {
            uint64_t pa, size;
            if (!r.read_be(pa) || !r.read_be(size)) return false;
            if (!pa && !size) return true;
            if (!visit(pa, size)) return false;
        }
    }
};
}
