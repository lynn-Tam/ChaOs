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
            || info->cap_count > MYOS_BOOTSTRAP_MAX_CAPS) {
            return libk::nullopt;
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

    [[nodiscard]] constexpr auto cpu_count() const noexcept -> uint32_t {
        return info_ == nullptr ? 0 : info_->cpu_count;
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
    explicit constexpr BootstrapView(
        const myos_bootstrap_info* info) noexcept
        : info_(info) {}

    const myos_bootstrap_info* info_{};
};

} // namespace myos::bootstrap
