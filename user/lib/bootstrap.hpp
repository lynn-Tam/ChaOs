#pragma once

/*
 * Borrowed interpretation of the fixed bootstrap envelope.  The kernel owns
 * the capability slots and the backing record; this view only validates the
 * immutable header and returns checked current-CSpace references.  It never
 * closes, duplicates or otherwise retains a capability.
 */

#include <stddef.h>
#include <stdint.h>

#include <libk/optional.hpp>
#include <uapi/bootstrap.h>
#include <user/lib/imports.hpp>
#include <user/lib/arguments.hpp>
#include <user/lib/capability.hpp>

namespace myos::bootstrap {

class BootstrapView final {
public:
    BootstrapView() noexcept = default;

    [[nodiscard]] static auto parse(
        const void* address,
        myos_word_t size) noexcept -> libk::optional<BootstrapView> {
        if (address == nullptr || size < sizeof(myos_bootstrap_info)) {
            return libk::nullopt;
        }
        const auto* const info = static_cast<const myos_bootstrap_info*>(
            address);
        if (info->magic != MYOS_BOOTSTRAP_MAGIC
            || info->major != MYOS_BOOTSTRAP_MAJOR
            || info->minor < MYOS_BOOTSTRAP_MINOR
            || info->size != sizeof(myos_bootstrap_info)
            || info->cap_count > MYOS_BOOTSTRAP_MAX_CAPS
            || info->import_count > MYOS_BOOTSTRAP_MAX_IMPORTS
            || info->reserved != 0 || !valid_arguments(info->arguments)) {
            return libk::nullopt;
        }
        for (uint32_t i = 0; i < info->cap_count; ++i) {
            const auto& entry = info->caps[i];
            if (entry.handle == 0 || entry.flags != 0
                || myos_bootstrap_object_kind(entry.kind) == MYOS_OBJECT_KIND_INVALID)
                return libk::nullopt;
            for (uint32_t j = 0; j < i; ++j)
                if (info->caps[j].kind == entry.kind) return libk::nullopt;
        }
        for (uint32_t i = 0; i < info->import_count; ++i) {
            const auto& entry = info->imports[i];
            if (entry.name[0] == 0 || entry.name[sizeof(entry.name) - 1] != 0
                || entry.handle == 0 || entry.protocol == 0 || entry.major == 0
                || entry.object_kind == MYOS_OBJECT_KIND_INVALID
                || entry.object_kind >= MYOS_OBJECT_KIND_COUNT
                || entry.flags != 0 || entry.reserved != 0) return libk::nullopt;
            for (uint32_t j = 0; j < i; ++j)
                if (equal(entry.name, info->imports[j].name)) return libk::nullopt;
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
        -> libk::optional<cap::CapRef> {
        if (!valid()) {
            return libk::nullopt;
        }
        for (uint32_t index = 0; index < info_->cap_count; ++index) {
            const myos_bootstrap_cap& entry = info_->caps[index];
            if (entry.kind == kind && entry.handle != 0) {
                return cap::CapRef{entry.handle, 0};
            }
        }
        return libk::nullopt;
    }

    [[nodiscard]] auto selector(uint32_t kind) const noexcept -> myos_cap_t {
        const auto reference = cap(kind);
        return reference ? reference->selector : 0;
    }

    // Missing optional imports return an empty reference. Required consumers
    // use service::capability, which rejects missing or incompatible bindings.
    [[nodiscard]] auto cap(Import requested) const noexcept
        -> libk::optional<cap::CapRef> {
        if (!valid() || requested.name == nullptr) return libk::nullopt;
        for (uint32_t i = 0; i < info_->import_count; ++i) {
            const auto& entry = info_->imports[i];
            if (!equal(entry.name, requested.name)) continue;
            if (entry.protocol != requested.protocol || entry.major != requested.major
                || entry.minor < requested.minor || entry.object_kind != requested.kind)
                return libk::nullopt;
            return cap::CapRef{entry.handle, 0};
        }
        return libk::nullopt;
    }

    [[nodiscard]] auto selector(Import requested) const noexcept -> myos_cap_t {
        const auto reference = cap(requested);
        return reference ? reference->selector : 0;
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
