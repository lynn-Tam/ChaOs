#pragma once

#include <stddef.h>
#include <uapi/start.h>
#include <stdint.h>
#include <optional>
#include <array>
#include <string_view>
#include <span>
#include <algorithm>
#include <sys/handle.hpp>
#include <sys/syscall.hpp>

namespace boot {

inline constexpr word_t event_badge = 2;

// Owned, bounded argument bytes. The wire form uses offsets, never pointers
// into the sender's address space. argv[0] names the selected application.
class args final {
    std::array<char, 256> bytes_{};
    size_t size_{};
public:
    static constexpr size_t max = 16;
    auto data() const noexcept -> const char* { return bytes_.data(); }
    auto size() const noexcept -> size_t { return size_; }
    auto count() const noexcept -> size_t {
        return std::count(bytes_.begin(), bytes_.begin() + size_, '\0');
    }
    auto append(const char* text, size_t n) noexcept -> bool {
        if (!text || n >= bytes_.size() - size_ || count() == max
            || std::find(text, text + n, '\0') != text + n) return false;
        std::copy_n(text, n, bytes_.begin() + size_);
        size_ += n;
        bytes_[size_++] = 0;
        return true;
    }
    auto decode(const char* text, size_t n) noexcept -> bool {
        size_ = 0;
        if (n > bytes_.size() || (n && (!text || text[n - 1] != 0))
            || (n && size_t(std::count(text, text + n, '\0')) > max)) return false;
        if (n) std::copy_n(text, n, bytes_.begin());
        size_ = n;
        return true;
    }
    auto argument(size_t i) const noexcept -> const char* {
        const auto end = bytes_.begin() + size_;
        auto p = bytes_.begin();
        while (i-- && p != end) p = std::find(p, end, '\0') + 1;
        return p == end ? nullptr : &*p;
    }
};

// Consumer contracts live in userspace. A protocol is independent of the
// local binding name; major must match and the provider minor must suffice.
struct Import final {
    const char* name;
    uint32_t protocol;
    uint16_t kind;
    uint16_t major{1};
    uint16_t minor{};
};

// Borrowed immutable records; names and protocol labels confer no authority.
class BootView final {
    const BootHdr* info_{};
    explicit constexpr BootView(const BootHdr* info) noexcept : info_(info) {}

  public:
    BootView() noexcept = default;
    auto entries() const noexcept -> std::span<const BootCap> {
        return info_ ? std::span{reinterpret_cast<const BootCap*>(info_ + 1), info_->count}
                     : std::span<const BootCap>{};
    }
    static auto parse(const void* address, word_t size) noexcept -> std::optional<BootView> {
        if (!address || reinterpret_cast<uintptr_t>(address) % alignof(BootHdr) ||
            size < sizeof(BootHdr))
            return std::nullopt;
        const auto* h = static_cast<const BootHdr*>(address);
        if (h->magic != BOOT_MAGIC || h->major != BOOT_MAJOR || h->size > size ||
            h->size < sizeof(BootHdr) || h->count > (h->size - sizeof(BootHdr)) / sizeof(BootCap))
            return std::nullopt;
        BootView view{h};
        const auto entries = view.entries();
        for (size_t i = 0; i < entries.size(); ++i) {
            const auto& e = entries[i];
            if (!e.handle || e.reserved || e.kind == ObjInvalid ||
                e.kind >= ObjCount || ((OBJ_KINDS >> e.kind) & 1) == 0)
                return std::nullopt;
            const bool resource = e.role == BOOT_RESOURCE;
            if (resource) {
                if (e.kind != ObjMem && e.kind != ObjIrq && e.kind != ObjHost) return std::nullopt;
            } else if (e.role == BOOT_FIRMWARE) {
                if (e.kind != ObjMem || !e.firmware.format || !e.bytes) return std::nullopt;
            } else if (e.role ? (boot_kind(e.role) != e.kind || e.contract.protocol ||
                                  e.contract.major || e.contract.minor || e.contract.name[0])
                             : (!e.contract.protocol || !e.contract.major || !e.contract.name[0] ||
                                  e.contract.name[BOOT_NAME_MAX - 1])) return std::nullopt;
            for (size_t j = 0; j < i; ++j) {
                const auto& prev = entries[j];
                if (resource ? (prev.role == BOOT_RESOURCE && prev.kind == e.kind &&
                                    prev.resource.node == e.resource.node && prev.resource.index == e.resource.index)
                             : (e.role ? e.role == prev.role
                                       : (!prev.role && std::string_view{e.contract.name} == prev.contract.name)))
                    return std::nullopt;
            }
        }
        return view;
    }
    auto valid() const noexcept -> bool { return info_ != nullptr; }
    auto data() const noexcept -> const void* { return info_; }
    auto size() const noexcept -> size_t { return info_ ? info_->size : 0; }
    auto cap(uint32_t role) const noexcept -> std::optional<cap_t> {
        for (const auto& e : entries())
            if (e.role == role && role)
                return e.handle;
        return std::nullopt;
    }
    auto resource(uint32_t node, obj_kind_t kind, uint32_t index = 0) const noexcept -> const BootCap* {
        for (const auto& e : entries())
            if (e.role == BOOT_RESOURCE && e.kind == kind && e.resource.node == node && e.resource.index == index)
                return &e;
        return nullptr;
    }
    auto record(uint32_t role) const noexcept -> const BootCap* {
        for (const auto& e : entries()) if (e.role == role) return &e;
        return nullptr;
    }
    auto find(Import req) const noexcept -> const BootCap* {
        for (const auto& e : entries())
            if (!e.role && req.name && std::string_view{e.contract.name} == req.name) {
                if (e.contract.protocol == req.protocol && e.contract.major == req.major && e.contract.minor >= req.minor &&
                    e.kind == req.kind) return &e;
                break;
            }
        return nullptr;
    }
    auto cap(Import req) const noexcept -> std::optional<cap_t> {
        const auto* e = find(req);
        return e ? std::optional{e->handle} : std::nullopt;
    }
    template <class T> auto selector(T req) const noexcept -> cap_t {
        const auto ref = cap(req);
        return ref ? *ref : 0;
    }
    auto cpu_count() const noexcept -> uint32_t { return info_ ? info_->cpu_count : 0; }
    auto stack_base() const noexcept -> uintptr_t { return info_ ? info_->stack_base : 0; }
    auto stack_size() const noexcept -> uint64_t { return info_ ? info_->stack_size : 0; }
    auto bundle_size() const noexcept -> uint64_t { return info_ ? info_->boot_bundle_size : 0; }
};
} // namespace boot

namespace sys {
inline auto bootstrap(const void* address, word_t size) noexcept -> boot::BootView {
    auto result = boot::BootView::parse(address, size);
    if (!result || result->cpu_count() == 0)
        sys::exit(STATUS_BAD_ARGS);
    return *result;
}
template <class Binding>
inline auto capability(const boot::BootView& info, Binding role) noexcept -> cap_t {
    auto cap = info.selector(role);
    if (cap == 0)
        sys::exit(STATUS_INVALID_CAP);
    return cap;
}


} // namespace sys
