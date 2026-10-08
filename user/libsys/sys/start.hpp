#pragma once

#include <stddef.h>
#include <uapi/start.h>
#include <stdint.h>
#include <optional>
#include <span>
#include <algorithm>
#include <sys/handle.hpp>
#include <sys/syscall.hpp>

namespace boot {

// Owned, bounded argument bytes. The wire form uses offsets, never pointers
// into the sender's address space. argv[0] names the selected application.
class Args final {
  public:
    static constexpr size_t Max = 16, Bytes = 256;
    struct Data {
        size_t count{}, size{};
        size_t offsets[Max]{};
        char bytes[Bytes]{};
    };

  private:
    Data args_{};

  public:
    auto append(const char* text, size_t size) noexcept -> bool {
        if (text == nullptr || args_.count == Max || size >= Bytes - args_.size)
            return false;
        for (size_t i = 0; i < size; ++i)
            if (text[i] == 0)
                return false;
        args_.offsets[args_.count++] = args_.size;
        for (size_t i = 0; i < size; ++i)
            args_.bytes[args_.size++] = text[i];
        args_.bytes[args_.size++] = 0;
        return true;
    }
    auto decode(const char* bytes, size_t size) noexcept -> bool {
        args_ = {};
        if (bytes == nullptr && size != 0)
            return false;
        for (size_t first = 0; first < size;) {
            size_t end = first;
            while (end < size && bytes[end] != 0)
                ++end;
            if (end == size || !append(bytes + first, end - first)) {
                args_ = {};
                return false;
            }
            first = end + 1;
        }
        return true;
    }
    auto data() const noexcept -> const Data& { return args_; }
    auto count() const noexcept -> size_t { return args_.count; }
    auto argument(size_t index) const noexcept -> const char* {
        return index < args_.count ? args_.bytes + args_.offsets[index] : nullptr;
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

inline constexpr Import UartMem{"uart.memory", 0x55415254, OBJECT_KIND_MEMORY};
inline constexpr Import UartIrq{"uart.irq", 0x55415254, OBJECT_KIND_IRQ};
inline constexpr Import ConsoleOutput{"console.output", 0x434f4e53, OBJECT_KIND_CHANNEL, 1, 1};
inline constexpr Import ConsoleInput{"console.input", 0x434f4e53, OBJECT_KIND_CHANNEL};
inline constexpr Import Stdin{"stdin", 0x5354524d, OBJECT_KIND_CHANNEL};
inline constexpr Import Stdout{"stdout", 0x5354524d, OBJECT_KIND_CHANNEL};
inline constexpr Import Stderr{"stderr", 0x5354524d, OBJECT_KIND_CHANNEL};
inline constexpr Import Process{"process", 0x50524f43, OBJECT_KIND_CHANNEL, 3, 1};
inline constexpr Import Files{"files", 0x46494c45, OBJECT_KIND_CHANNEL, 3};
inline constexpr Import FilesRead{"files.read", 0x46494c45, OBJECT_KIND_CHANNEL, 3};
inline constexpr Import Block{"block", 0x424c4f43, OBJECT_KIND_CHANNEL, 2, 1};
inline constexpr Import Store{"store", 0x53544f52, OBJECT_KIND_CHANNEL, 1, 1};
inline constexpr Import StoreRead{"store.read", 0x53544f52, OBJECT_KIND_CHANNEL, 1, 1};
inline constexpr Import StoreAdmin{"store.admin", 0x53544f52, OBJECT_KIND_CHANNEL, 1, 1};
inline constexpr Import Vfs{"vfs", 0x56465320, OBJECT_KIND_CHANNEL};
inline constexpr Import VfsRead{"vfs.read", 0x56465320, OBJECT_KIND_CHANNEL};
inline constexpr Import ServiceControl{"service.control", 0x53564354, OBJECT_KIND_CHANNEL};
inline constexpr Import ServiceWake{"service.wake", 0x53564357, OBJECT_KIND_NOTIFICATION};
inline constexpr Import Pager{"pager", 0x50414745, OBJECT_KIND_PAGER};
inline constexpr Import TargetMemory{"target.memory", 0x4d454d4f, OBJECT_KIND_MEMORY};
inline constexpr Import StagingMemory{"staging.memory", 0x4d454d4f, OBJECT_KIND_MEMORY};
inline constexpr Import StagingRegion{"staging.region", 0x56535043, OBJECT_KIND_VSPACE};
// Borrowed immutable records; names and protocol labels confer no authority.
class BootView final {
    const BootHdr* info_{};
    explicit constexpr BootView(const BootHdr* info) noexcept : info_(info) {}
    static auto equal(const char* a, const char* b) noexcept -> bool {
        while (*a && *a == *b) {
            ++a;
            ++b;
        }
        return *a == *b;
    }

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
            if (!e.handle || e.reserved || e.kind == OBJECT_KIND_INVALID ||
                e.kind >= OBJECT_KIND_COUNT || ((OBJECT_KINDS >> e.kind) & 1) == 0)
                return std::nullopt;
            if (e.role
                    ? (boot_kind(e.role) != e.kind || e.protocol || e.major || e.minor || e.name[0])
                    : (!e.protocol || !e.major || !e.name[0] || e.name[BOOT_NAME_MAX - 1]))
                return std::nullopt;
            for (size_t j = 0; j < i; ++j)
                if (e.role ? e.role == entries[j].role
                           : (!entries[j].role && equal(e.name, entries[j].name)))
                    return std::nullopt;
        }
        return view;
    }
    auto valid() const noexcept -> bool { return info_ != nullptr; }
    auto data() const noexcept -> const void* { return info_; }
    auto size() const noexcept -> size_t { return info_ ? info_->size : 0; }
    auto cap(uint32_t role) const noexcept -> std::optional<sys::cap::CapRef> {
        for (const auto& e : entries())
            if (e.role == role && role)
                return sys::cap::CapRef{e.handle, 0};
        return std::nullopt;
    }
    auto find(Import req) const noexcept -> const BootCap* {
        for (const auto& e : entries())
            if (!e.role && req.name && equal(e.name, req.name)) {
                if (e.protocol == req.protocol && e.major == req.major && e.minor >= req.minor &&
                    e.kind == req.kind) return &e;
                break;
            }
        return nullptr;
    }
    auto cap(Import req) const noexcept -> std::optional<sys::cap::CapRef> {
        const auto* e = find(req);
        return e ? std::optional{sys::cap::CapRef{e->handle, 0}} : std::nullopt;
    }
    template <class T> auto selector(T req) const noexcept -> cap_t {
        const auto ref = cap(req);
        return ref ? ref->selector : 0;
    }
    auto cpu_count() const noexcept -> uint32_t { return info_ ? info_->cpu_count : 0; }
    auto stack_base() const noexcept -> uintptr_t { return info_ ? info_->stack_base : 0; }
    auto stack_size() const noexcept -> uint64_t { return info_ ? info_->stack_size : 0; }
    auto bundle_size() const noexcept -> uint64_t { return info_ ? info_->boot_bundle_size : 0; }
};
} // namespace boot

namespace sys::service {
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


} // namespace sys::service
