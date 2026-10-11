#pragma once

/*
 * BootBundle is a little-endian wire format. These are byte offsets, not a C
 * object layout: readers must decode fields explicitly from bounded storage.
 */
#define PKG_MAGIC UINT64_C(0x3230474b504b554d) /* "MUKPKG02" */
#define PKG_MAJOR 2
#define PKG_MINOR 0

#define PKG_ARCH_RISCV64 1
#define PKG_ABI_RISCV_LP64 1

#define PKG_HEADER_SIZE 80
#define PKG_MODULE_SIZE 64
#define PKG_SEGMENT_SIZE 40

#define PKG_MODULE_BOOTABLE (UINT32_C(1) << 0)
#define PKG_MODULE_DATA     (UINT32_C(1) << 1)
#define PKG_MODULE_ROLE_MASK \
    (PKG_MODULE_BOOTABLE | PKG_MODULE_DATA)

#define PKG_SEGMENT_READ    (UINT32_C(1) << 0)
#define PKG_SEGMENT_WRITE   (UINT32_C(1) << 1)
#define PKG_SEGMENT_EXECUTE (UINT32_C(1) << 2)

#include <libk/byte_reader.hpp>
#include <optional>

// Bootpack and its consumers share this internal package format, not a syscall
// ABI. The kernel reads only the selected root's segment range and entry.
#define BOOT_LOAD_MAGIC UINT64_C(0x3230544f4f425652)
#define BOOT_LOAD_TRAILER 40U

namespace libk {
struct load_seg {
    uint64_t va, off, file, size, perms;
    static constexpr auto read(byte_reader& r) noexcept -> std::optional<load_seg> {
        load_seg s{};
        if (!r.read_le(s.va) || !r.read_le(s.off) || !r.read_le(s.file) ||
            !r.read_le(s.size) || !r.read_le(s.perms)) return {};
        return s;
    }
};

// Borrowed package bytes. A view never owns mappings, capabilities or policy.
struct pkg_seg {
    uint64_t va{};
    std::span<const uint8_t> file{};
    uint64_t size{}, perms{};
};
struct pkg_mod {
    std::string_view name{};
    std::span<const uint8_t> bytes{}, data{}, loads{};
    uint64_t entry{};
    bool executable{};
    auto segment_count() const noexcept -> size_t { return loads.size() / PKG_SEGMENT_SIZE; }
    auto segment(size_t i) const noexcept -> std::optional<pkg_seg> {
        if (i >= segment_count()) return {};
        byte_reader r{loads.data() + i * PKG_SEGMENT_SIZE, PKG_SEGMENT_SIZE};
        const auto s = load_seg::read(r);
        if (!s || !s->size || s->file > s->size || !s->perms || s->perms > 7 ||
            ((s->perms & PKG_SEGMENT_WRITE) && !(s->perms & PKG_SEGMENT_READ)) ||
            ((s->perms & PKG_SEGMENT_WRITE) && (s->perms & PKG_SEGMENT_EXECUTE)) ||
            s->va % 4096 || s->size > UINT64_MAX - 4095 ||
            ((s->size + 4095) & ~uint64_t{4095}) > UINT64_MAX - s->va ||
            s->off > bytes.size() || s->file > bytes.size() - s->off) return {};
        const auto file = bytes.subspan(s->off, s->file);
        // All file bytes must belong to this module's bounded payload.
        const auto base = static_cast<size_t>(data.data() - bytes.data());
        if (s->off < base || s->off - base > data.size() ||
            s->file > data.size() - (s->off - base)) return {};
        return pkg_seg{s->va, file, s->size, s->perms};
    }
};

class pkg_view {
    std::span<const uint8_t> bytes_{}, mods_{}, loads_{};
    uint32_t root_{};
public:
    explicit operator bool() const noexcept { return !bytes_.empty(); }
    auto size() const noexcept -> size_t { return bytes_.size(); }
    auto module_count() const noexcept -> size_t { return mods_.size() / PKG_MODULE_SIZE; }
    auto module(size_t i) const noexcept -> std::optional<pkg_mod> {
        if (i >= module_count()) return {};
        byte_reader r{mods_.data() + i * PKG_MODULE_SIZE, PKG_MODULE_SIZE};
        uint64_t name{}, off{}, size{}, entry{}, tls{}, tls_size{};
        uint32_t len{}, flags{}, first{}, count{};
        if (!r.read_le(name) || !r.read_le(len) || !r.read_le(flags) ||
            !r.read_le(off) || !r.read_le(size) || !r.read_le(entry) ||
            !r.read_le(first) || !r.read_le(count) || !r.read_le(tls) || !r.read_le(tls_size) ||
            !len || !size || tls || tls_size ||
            (flags != PKG_MODULE_BOOTABLE && flags != PKG_MODULE_DATA) ||
            name > bytes_.size() || len > bytes_.size() - name ||
            off > bytes_.size() || size > bytes_.size() - off ||
            first > loads_.size() / PKG_SEGMENT_SIZE ||
            count > loads_.size() / PKG_SEGMENT_SIZE - first) return {};
        const std::string_view label{reinterpret_cast<const char*>(bytes_.data() + name), len};
        if (label.find('\0') != label.npos) return {};
        return pkg_mod{label, bytes_, bytes_.subspan(off, size),
            loads_.subspan(size_t{first} * PKG_SEGMENT_SIZE, size_t{count} * PKG_SEGMENT_SIZE),
            entry, flags == PKG_MODULE_BOOTABLE};
    }
    auto find(std::string_view name) const noexcept -> std::optional<pkg_mod> {
        for (size_t i = 0; i < module_count(); ++i) {
            auto m = module(i);
            if (m && m->name == name) return m;
        }
        return {};
    }
    auto root_is(std::string_view name) const noexcept -> bool {
        const auto m = module(root_);
        return m && m->name == name;
    }
    static auto parse(const void* data, size_t size) noexcept -> pkg_view {
        if (!data || size < PKG_HEADER_SIZE) return {};
        byte_reader r{static_cast<const uint8_t*>(data), size};
        uint64_t magic{}, total{}, features{}, mods{}, loads{}, checksum{};
        uint16_t major{}, minor{};
        uint32_t header{}, arch{}, abi{}, count{}, root{}, segments{}, reserved{};
        if (!r.read_le(magic) || !r.read_le(major) || !r.read_le(minor) ||
            !r.read_le(header) || !r.read_le(total) || !r.read_le(arch) || !r.read_le(abi) ||
            !r.read_le(features) || !r.read_le(mods) || !r.read_le(count) || !r.read_le(root) ||
            !r.read_le(loads) || !r.read_le(segments) || !r.read_le(reserved) || !r.read_le(checksum) ||
            magic != PKG_MAGIC || major != PKG_MAJOR || minor != PKG_MINOR ||
            header != PKG_HEADER_SIZE || total != size || arch != PKG_ARCH_RISCV64 ||
            abi != PKG_ABI_RISCV_LP64 || features || reserved || checksum ||
            !count || root >= count || mods < header || mods > size ||
            count > (size - mods) / PKG_MODULE_SIZE ||
            loads < mods + uint64_t{count} * PKG_MODULE_SIZE || loads > size ||
            segments > (size - loads) / PKG_SEGMENT_SIZE) return {};
        pkg_view view;
        view.bytes_ = {static_cast<const uint8_t*>(data), size};
        view.mods_ = view.bytes_.subspan(mods, size_t{count} * PKG_MODULE_SIZE);
        view.loads_ = view.bytes_.subspan(loads, size_t{segments} * PKG_SEGMENT_SIZE);
        view.root_ = root;
        size_t first{};
        for (size_t i = 0; i < count; ++i) {
            const auto m = view.module(i);
            if (!m || m->loads.data() != view.loads_.data() + first * PKG_SEGMENT_SIZE) return {};
            if (!m->executable) {
                if (m->entry || !m->loads.empty()) return {};
                continue;
            }
            if (m->loads.empty()) return {};
            first += m->segment_count();
            uint64_t end{};
            bool entry{};
            for (size_t j = 0; j < m->segment_count(); ++j) {
                const auto s = m->segment(j);
                if (!s || s->va < end) return {};
                end = s->va + ((s->size + 4095) & ~uint64_t{4095});
                entry |= m->entry >= s->va && m->entry - s->va < s->size &&
                         (s->perms & PKG_SEGMENT_EXECUTE);
            }
            if (!entry) return {};
        }
        if (first != segments || !view.module(root)->executable) return {};
        return view;
    }
};
} // namespace libk
