#pragma once

#include <stddef.h>
#include <uapi/bootstrap.h>
#include <stdint.h>
#include <optional>
#include <sys/handle.hpp>
#include <sys/syscall.hpp>

namespace myos::bootstrap {

inline auto valid_arguments(const myos_bootstrap_arguments& args) noexcept -> bool {
    if (args.count > MYOS_BOOTSTRAP_ARG_MAX || args.size > MYOS_BOOTSTRAP_ARG_BYTES) return false;
    size_t cursor{};
    for (size_t i = 0; i < args.count; ++i) {
        if (args.offsets[i] != cursor) return false;
        while (cursor < args.size && args.bytes[cursor] != 0) ++cursor;
        if (cursor == args.size) return false;
        ++cursor;
    }
    return cursor == args.size;
}

// Owned, bounded argument bytes. The wire form uses offsets, never pointers
// into the sender's address space. argv[0] names the selected application.
class Arguments final {
    myos_bootstrap_arguments args_{};
public:
    auto append(const char* text, size_t size) noexcept -> bool {
        if (text == nullptr || args_.count == MYOS_BOOTSTRAP_ARG_MAX
            || size >= MYOS_BOOTSTRAP_ARG_BYTES - args_.size) return false;
        for (size_t i = 0; i < size; ++i) if (text[i] == 0) return false;
        args_.offsets[args_.count++] = args_.size;
        for (size_t i = 0; i < size; ++i) args_.bytes[args_.size++] = text[i];
        args_.bytes[args_.size++] = 0;
        return true;
    }
    auto decode(const char* bytes, size_t size) noexcept -> bool {
        args_ = {};
        for (size_t first = 0; first < size;) {
            size_t end = first;
            while (end < size && bytes[end] != 0) ++end;
            if (end == size || !append(bytes + first, end - first)) { args_ = {}; return false; }
            first = end + 1;
        }
        return true;
    }
    auto data() const noexcept -> const myos_bootstrap_arguments& { return args_; }
    auto count() const noexcept -> size_t { return args_.count; }
    auto argument(size_t index) const noexcept -> const char* {
        return index < args_.count ? args_.bytes + args_.offsets[index] : nullptr;
    }
};
} // namespace myos::bootstrap

namespace myos::bootstrap {

// Consumer contracts live in userspace. A protocol is independent of the
// local binding name; major must match and the provider minor must suffice.
struct Import final {
    const char* name;
    uint32_t protocol;
    uint16_t kind;
    uint16_t major{1};
    uint16_t minor{};
};

namespace imports {
inline constexpr Import ConsoleOutput{"console.output", 0x434f4e53, MYOS_OBJECT_KIND_CHANNEL, 1, 1};
inline constexpr Import ConsoleInput{"console.input", 0x434f4e53, MYOS_OBJECT_KIND_CHANNEL};
inline constexpr Import Stdin{"stdin", 0x5354524d, MYOS_OBJECT_KIND_CHANNEL};
inline constexpr Import Stdout{"stdout", 0x5354524d, MYOS_OBJECT_KIND_CHANNEL};
inline constexpr Import Stderr{"stderr", 0x5354524d, MYOS_OBJECT_KIND_CHANNEL};
inline constexpr Import Process{"process", 0x50524f43, MYOS_OBJECT_KIND_CHANNEL, 3, 1};
inline constexpr Import Files{"files", 0x46494c45, MYOS_OBJECT_KIND_CHANNEL, 3};
inline constexpr Import FilesRead{"files.read", 0x46494c45, MYOS_OBJECT_KIND_CHANNEL, 3};
inline constexpr Import Block{"block", 0x424c4f43, MYOS_OBJECT_KIND_CHANNEL, 2, 1};
inline constexpr Import Store{"store", 0x53544f52, MYOS_OBJECT_KIND_CHANNEL, 1, 1};
inline constexpr Import StoreRead{"store.read", 0x53544f52, MYOS_OBJECT_KIND_CHANNEL, 1, 1};
inline constexpr Import StoreAdmin{"store.admin", 0x53544f52, MYOS_OBJECT_KIND_CHANNEL, 1, 1};
inline constexpr Import Vfs{"vfs", 0x56465320, MYOS_OBJECT_KIND_CHANNEL};
inline constexpr Import VfsRead{"vfs.read", 0x56465320, MYOS_OBJECT_KIND_CHANNEL};
inline constexpr Import ServiceControl{"service.control", 0x53564354, MYOS_OBJECT_KIND_CHANNEL};
inline constexpr Import ServiceWake{"service.wake", 0x53564357, MYOS_OBJECT_KIND_NOTIFICATION};
inline constexpr Import Pager{"pager", 0x50414745, MYOS_OBJECT_KIND_PAGER};
inline constexpr Import TargetMemory{"target.memory", 0x4d454d4f, MYOS_OBJECT_KIND_MEMORY};
inline constexpr Import StagingMemory{"staging.memory", 0x4d454d4f, MYOS_OBJECT_KIND_MEMORY};
inline constexpr Import StagingRegion{"staging.region", 0x56535043, MYOS_OBJECT_KIND_VSPACE};
} // namespace imports
} // namespace myos::bootstrap

/*
 * Borrowed interpretation of the fixed bootstrap envelope.  The kernel owns
 * the capability slots and the backing record; this view only validates the
 * immutable header and returns checked current-CSpace references.  It never
 * closes, duplicates or otherwise retains a capability.
 */



namespace myos::bootstrap {

class BootstrapView final {
public:
    BootstrapView() noexcept = default;

    [[nodiscard]] static auto parse(
        const void* address,
        myos_word_t size) noexcept -> std::optional<BootstrapView> {
        if (address == nullptr || size < sizeof(myos_bootstrap_info)) {
            return std::nullopt;
        }
        const auto* const info = static_cast<const myos_bootstrap_info*>(
            address);
        if (info->magic != MYOS_BOOTSTRAP_MAGIC
            || info->major != MYOS_BOOTSTRAP_MAJOR
            || info->minor < MYOS_BOOTSTRAP_MINOR
            || info->size < sizeof(myos_bootstrap_info)
            || info->size > size
            || info->cap_count > MYOS_BOOTSTRAP_MAX_CAPS
            || info->import_count > MYOS_BOOTSTRAP_MAX_IMPORTS
            || info->reserved != 0 || !valid_arguments(info->arguments)) {
            return std::nullopt;
        }
        for (uint32_t i = 0; i < info->cap_count; ++i) {
            const auto& entry = info->caps[i];
            if (entry.handle == 0 || entry.flags != 0
                || myos_bootstrap_object_kind(entry.kind) == MYOS_OBJECT_KIND_INVALID)
                return std::nullopt;
            for (uint32_t j = 0; j < i; ++j)
                if (info->caps[j].kind == entry.kind) return std::nullopt;
        }
        for (uint32_t i = 0; i < info->import_count; ++i) {
            const auto& entry = info->imports[i];
            if (entry.name[0] == 0 || entry.name[sizeof(entry.name) - 1] != 0
                || entry.handle == 0 || entry.protocol == 0 || entry.major == 0
                || entry.object_kind == MYOS_OBJECT_KIND_INVALID
                || entry.object_kind >= MYOS_OBJECT_KIND_COUNT
                || ((MYOS_OBJECT_KINDS >> entry.object_kind) & 1) == 0
                || entry.flags != 0 || entry.reserved != 0) return std::nullopt;
            for (uint32_t j = 0; j < i; ++j)
                if (equal(entry.name, info->imports[j].name)) return std::nullopt;
        }
        return BootstrapView{info};
    }

    [[nodiscard]] constexpr auto valid() const noexcept -> bool {
        return info_ != nullptr;
    }

    [[nodiscard]] constexpr auto data() const noexcept -> const void* {
        return info_;
    }

    [[nodiscard]] auto cap(uint32_t kind) const noexcept
        -> std::optional<cap::CapRef> {
        if (!valid()) {
            return std::nullopt;
        }
        for (uint32_t index = 0; index < info_->cap_count; ++index) {
            const myos_bootstrap_cap& entry = info_->caps[index];
            if (entry.kind == kind && entry.handle != 0) {
                return cap::CapRef{entry.handle, 0};
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] auto selector(uint32_t kind) const noexcept -> myos_cap_t {
        const auto reference = cap(kind);
        return reference ? reference->selector : 0;
    }

    // Missing optional imports return an empty reference. Required consumers
    // use service::capability, which rejects missing or incompatible bindings.
    [[nodiscard]] auto cap(Import requested) const noexcept
        -> std::optional<cap::CapRef> {
        if (!valid() || requested.name == nullptr) return std::nullopt;
        for (uint32_t i = 0; i < info_->import_count; ++i) {
            const auto& entry = info_->imports[i];
            if (!equal(entry.name, requested.name)) continue;
            if (entry.protocol != requested.protocol || entry.major != requested.major
                || entry.minor < requested.minor || entry.object_kind != requested.kind)
                return std::nullopt;
            return cap::CapRef{entry.handle, 0};
        }
        return std::nullopt;
    }

    [[nodiscard]] auto selector(Import requested) const noexcept -> myos_cap_t {
        const auto reference = cap(requested);
        return reference ? reference->selector : 0;
    }

    // Initial hardware authority is a finite boot inventory. Each entry is
    // a distinct Device grant; DEVICE_INFO remains the identity source.
    [[nodiscard]] auto device_count() const noexcept -> size_t {
        if (!valid()) return 0;
        size_t count{};
        for (uint32_t i = 0; i < info_->import_count; ++i)
            if (info_->imports[i].protocol == MYOS_BOOTSTRAP_DEVICE_PROTOCOL
                && info_->imports[i].object_kind == MYOS_OBJECT_KIND_DEVICE
                && info_->imports[i].major == 1) ++count;
        return count;
    }
    [[nodiscard]] auto device_import(size_t ordinal) const noexcept
        -> const myos_bootstrap_import* {
        if (!valid()) return nullptr;
        for (uint32_t i = 0; i < info_->import_count; ++i) {
            const auto& entry = info_->imports[i];
            if (entry.protocol == MYOS_BOOTSTRAP_DEVICE_PROTOCOL
                && entry.object_kind == MYOS_OBJECT_KIND_DEVICE
                && entry.major == 1) {
                if (ordinal == 0) return &entry;
                --ordinal;
            }
        }
        return nullptr;
    }
    [[nodiscard]] auto device(size_t ordinal) const noexcept -> myos_cap_t {
        const auto* entry = device_import(ordinal);
        return entry == nullptr ? 0 : entry->handle;
    }

    [[nodiscard]] constexpr auto cpu_count() const noexcept -> uint32_t {
        return info_ == nullptr ? 0 : info_->cpu_count;
    }
    [[nodiscard]] auto argument_count() const noexcept -> size_t {
        return info_ == nullptr ? 0 : info_->arguments.count;
    }
    [[nodiscard]] auto argument(size_t index) const noexcept -> const char* {
        return index < argument_count() ? info_->arguments.bytes + info_->arguments.offsets[index] : nullptr;
    }

    [[nodiscard]] constexpr auto stack_base() const noexcept -> uintptr_t {
        return info_ == nullptr ? 0 : info_->stack_base;
    }

    [[nodiscard]] constexpr auto stack_size() const noexcept -> uint64_t {
        return info_ == nullptr ? 0 : info_->stack_size;
    }

    [[nodiscard]] constexpr auto bundle_size() const noexcept -> uint64_t {
        return info_ == nullptr ? 0 : info_->boot_bundle_size;
    }

private:
    static auto equal(const char* a, const char* b) noexcept -> bool {
        while (*a != 0 && *a == *b) { ++a; ++b; }
        return *a == *b;
    }
    explicit constexpr BootstrapView(
        const myos_bootstrap_info* info) noexcept
        : info_(info) {}

    const myos_bootstrap_info* info_{};
};

} // namespace myos::bootstrap

namespace myos::service {
inline auto bootstrap(const void* address, myos_word_t size) noexcept -> bootstrap::BootstrapView {
    auto result = bootstrap::BootstrapView::parse(address, size);
    if (!result || result->cpu_count() == 0) myos::exit(MYOS_STATUS_BAD_ARGS);
    return *result;
}
template<class Binding>
inline auto capability(const bootstrap::BootstrapView& info, Binding role) noexcept -> myos_cap_t {
    auto cap = info.selector(role);
    if (cap == 0) myos::exit(MYOS_STATUS_INVALID_CAP);
    return cap;
}
inline auto initial_device(const bootstrap::BootstrapView& info,
    size_t ordinal = 0) noexcept -> myos_cap_t {
    const auto cap = info.device(ordinal);
    if (cap == 0) myos::exit(MYOS_STATUS_NOT_FOUND);
    return cap;
}

} // namespace myos::service
