#pragma once
#include <cstdint>
#include <cstddef>
#include <span>
#include <optional>
#include <bit>

namespace libk {
static_assert(std::endian::native == std::endian::little);
// ELF64 little-endian static executables. This view borrows the original file;
// machine selection, address-space limits and mapping policy belong to callers.
struct elf_seg {
    uint32_t type{}, flags{};
    uint64_t offset{}, va{}, pa{}, file_size{}, size{}, align{};
};
static_assert(sizeof(elf_seg) == 56);
class elf_view {
    struct header {
        uint8_t ident[16];
        uint16_t type, machine;
        uint32_t version;
        uint64_t entry, phoff, shoff;
        uint32_t flags;
        uint16_t ehsize, phsize, phnum, shsize, shnum, shstr;
    };
    static_assert(sizeof(header) == 64);
    std::span<const std::byte> bytes_;
    header h_{};
public:
    auto bytes() const noexcept { return bytes_; }
    auto entry() const noexcept { return h_.entry; }
    auto machine() const noexcept { return h_.machine; }
    auto segment_count() const noexcept { return h_.phnum; }
    auto segment(size_t i) const noexcept -> std::optional<elf_seg> {
        if (i >= h_.phnum) return {};
        elf_seg s;
        __builtin_memcpy(&s, bytes_.data() + h_.phoff + i * sizeof(s), sizeof(s));
        return s;
    }
    explicit operator bool() const noexcept { return !bytes_.empty(); }
    static auto parse(std::span<const std::byte> bytes) noexcept -> elf_view {
        elf_view v;
        if (bytes.size() < sizeof(header)) return {};
        __builtin_memcpy(&v.h_, bytes.data(), sizeof(header));
        const auto& h = v.h_;
        if (h.ident[0] != 0x7f || h.ident[1] != 'E' || h.ident[2] != 'L' || h.ident[3] != 'F' ||
            h.ident[4] != 2 || h.ident[5] != 1 || h.ident[6] != 1 || h.type != 2 || h.version != 1 ||
            h.ehsize != sizeof(header) || h.phsize != sizeof(elf_seg) || !h.phnum || h.phnum == 0xffff ||
            h.phoff > bytes.size() || h.phnum > (bytes.size() - h.phoff) / sizeof(elf_seg)) return {};
        v.bytes_ = bytes;
        bool entry{};
        std::optional<elf_seg> tls;
        for (size_t i = 0; i < h.phnum; ++i) {
            const auto s = *v.segment(i);
            // Static TLS is initialized by the CRT; dynamic images remain unsupported.
            if (s.type == 2 || s.type == 3) return {};
            if (s.type == 7) { if (tls) return {}; tls = s; }
            if (s.type != 1 && s.type != 7) continue;
            if (s.file_size > s.size || s.offset > bytes.size() || s.file_size > bytes.size() - s.offset ||
                s.size > UINT64_MAX - s.va || (s.flags & ~7u) || (s.flags & 3u) == 3u ||
                (s.align > 1 && ((s.align & (s.align - 1)) ||
                    (s.va & (s.align - 1)) != (s.offset & (s.align - 1))))) return {};
            entry |= s.type == 1 && (s.flags & 1) && h.entry >= s.va && h.entry - s.va < s.size;
        }
        if (tls && tls->file_size) {
            bool mapped{};
            for (size_t i = 0; i < h.phnum; ++i) {
                const auto s = *v.segment(i);
                if (s.type != 1 || !(s.flags & 4) || tls->va < s.va || tls->offset < s.offset) continue;
                const auto offset = tls->va - s.va;
                mapped |= offset == tls->offset - s.offset && offset <= s.file_size
                    && tls->file_size <= s.file_size - offset;
            }
            if (!mapped) return {};
        }
        return entry ? v : elf_view{};
    }
};
}
