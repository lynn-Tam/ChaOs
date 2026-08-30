#pragma once

/*
 * Userspace admission relation for the public capability attenuation ABI.
 *
 * This is deliberately expressed only in the UAPI descriptor vocabulary.  It
 * is shared by ManifestView and the Unit 3 authority/import path; the kernel
 * still decodes and checks the actual source authority at every syscall.
 */

#include <stddef.h>
#include <stdint.h>

#include <uapi/capability.h>
#include <uapi/deploy.h>
#include <uapi/endpoint.h>
#include <uapi/object.h>
#include <uapi/vm.h>

namespace myos::deploy::attenuation {

enum class DescriptorForm : uint8_t {
    Ceiling,
    DuplicateRequest,
    TypedRequest,
};

[[nodiscard]] constexpr auto zero_words(
    const myos_cap_attenuation& value,
    uint32_t first) noexcept -> bool {
    for (uint32_t index = first; index < 6; ++index) {
        if (value.words[index] != 0) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr auto valid_kind(uint16_t kind) noexcept -> bool {
    return kind > MYOS_OBJECT_KIND_INVALID && kind < MYOS_OBJECT_KIND_COUNT;
}

[[nodiscard]] constexpr auto valid_access(uint64_t access) noexcept -> bool {
    return (access & ~(uint64_t{MYOS_VM_READ}
                       | uint64_t{MYOS_VM_WRITE}
                       | uint64_t{MYOS_VM_EXECUTE})) == 0
        && access != 0
        && ((access & MYOS_VM_WRITE) == 0
            || (access & MYOS_VM_READ) != 0);
}

[[nodiscard]] constexpr auto valid_types(uint64_t types) noexcept -> bool {
    constexpr uint64_t valid = uint64_t{MYOS_VM_NORMAL}
        | uint64_t{MYOS_VM_UNCACHED} | uint64_t{MYOS_VM_DEVICE};
    return types != 0 && (types & ~valid) == 0;
}

[[nodiscard]] constexpr auto valid_rights(uint64_t rights) noexcept -> bool {
    return (rights & ~uint64_t{MYOS_RIGHT_MASK}) == 0;
}

[[nodiscard]] constexpr auto valid_range(
    uint64_t base,
    uint64_t count) noexcept -> bool {
    return count != 0 && base <= UINT64_MAX - count;
}

/*
 * Validate one canonical descriptor form.  A registered ceiling may carry a
 * rights-only Tunnel source; Duplicate is the rights-only wire form for every
 * kind; TypedRequest is the full typed form and deliberately excludes Tunnel,
 * whose only accepted current operation is Duplicate.
 */
[[nodiscard]] constexpr auto valid_descriptor(
    const myos_cap_attenuation& value,
    DescriptorForm form = DescriptorForm::Ceiling) noexcept -> bool {
    if (value.version != MYOS_CAP_ATTENUATION_VERSION_CURRENT
        || value.size != MYOS_CAP_ATTENUATION_SIZE
        || !valid_kind(value.kind) || !valid_rights(value.rights)) {
        return false;
    }
    if (form == DescriptorForm::DuplicateRequest) {
        return zero_words(value, 0);
    }

    switch (value.kind) {
    case MYOS_OBJECT_KIND_THREAD:
    case MYOS_OBJECT_KIND_SCHED_CONTEXT:
    case MYOS_OBJECT_KIND_SCHED_DOMAIN:
    case MYOS_OBJECT_KIND_CSPACE:
    case MYOS_OBJECT_KIND_NOTIFICATION:
    case MYOS_OBJECT_KIND_VPROC:
    case MYOS_OBJECT_KIND_IRQ:
        return zero_words(value, 0);
    case MYOS_OBJECT_KIND_TUNNEL:
        return form != DescriptorForm::TypedRequest
            && zero_words(value, 0);
    case MYOS_OBJECT_KIND_MEMORY:
        return valid_range(value.words[0], value.words[1])
            && valid_access(value.words[2])
            && valid_types(value.words[3])
            && zero_words(value, 4);
    case MYOS_OBJECT_KIND_VSPACE:
        return value.words[0] != 0 && value.words[1] != 0
            && (value.words[0] % MYOS_DEPLOY_PAGE_SIZE) == 0
            && (value.words[1] % MYOS_DEPLOY_PAGE_SIZE) == 0
            && valid_range(value.words[0], value.words[1])
            && valid_access(value.words[2])
            && valid_types(value.words[3])
            && zero_words(value, 4);
    case MYOS_OBJECT_KIND_RESOURCE_POOL: {
        constexpr uint64_t valid_mask =
            (uint64_t{1} << MYOS_OBJECT_KIND_COUNT) - 1;
        return (value.words[2] & ~valid_mask) == 0
            && (value.words[2] & (uint64_t{1}
                                  << MYOS_OBJECT_KIND_INVALID)) == 0
            && zero_words(value, 3);
    }
    case MYOS_OBJECT_KIND_ENDPOINT:
        return (value.words[0] & ~value.words[1]) == 0
            && value.words[2] <= MYOS_ENDPOINT_MAX_CAPS
            && zero_words(value, 3);
    case MYOS_OBJECT_KIND_CHANNEL: {
        const bool unbound = value.words[1] == 0 && value.words[2] == 0;
        const bool exact = value.words[1] != 0
            && value.words[2] == UINT64_MAX;
        return value.words[0] <= MYOS_CAP_CHANNEL_SIDE_B
            && (unbound || exact) && zero_words(value, 3);
    }
    case MYOS_OBJECT_KIND_PAGER:
        return value.words[0] != 0 && zero_words(value, 1);
    case MYOS_OBJECT_KIND_INVALID:
    case MYOS_OBJECT_KIND_COUNT:
        return false;
    }
    return false;
}

/* Serialize one decoded descriptor through the canonical UAPI offsets.  The
 * deployment parser and the kernel both consume this fixed little-endian
 * representation; callers must not copy the native C++ object layout into a
 * descriptor MemoryObject. */
inline void encode_wire(
    const myos_cap_attenuation& value,
    uint8_t (&output)[MYOS_CAP_ATTENUATION_SIZE]) noexcept {
    for (size_t index = 0; index < MYOS_CAP_ATTENUATION_SIZE; ++index) {
        output[index] = 0;
    }
    const auto put = [&output](
        size_t offset, uint64_t field, size_t width) noexcept {
        for (size_t byte = 0; byte < width; ++byte) {
            output[offset + byte] = static_cast<uint8_t>(field >> (byte * 8));
        }
    };
    put(MYOS_CAP_ATTENUATION_VERSION_OFFSET, value.version, 2);
    put(MYOS_CAP_ATTENUATION_KIND_OFFSET, value.kind, 2);
    put(MYOS_CAP_ATTENUATION_SIZE_OFFSET, value.size, 4);
    put(MYOS_CAP_ATTENUATION_RIGHTS_OFFSET, value.rights, 8);
    for (size_t index = 0; index < 6; ++index) {
        put(MYOS_CAP_ATTENUATION_WORD0_OFFSET + index * 8,
            value.words[index], 8);
    }
}

[[nodiscard]] constexpr auto rights_within(
    const myos_cap_attenuation& requested,
    const myos_cap_attenuation& ceiling) noexcept -> bool {
    return requested.kind == ceiling.kind
        && (requested.rights & ~ceiling.rights) == 0;
}

[[nodiscard]] constexpr auto range_within(
    uint64_t outer_base,
    uint64_t outer_count,
    uint64_t inner_base,
    uint64_t inner_count) noexcept -> bool {
    return valid_range(outer_base, outer_count)
        && valid_range(inner_base, inner_count)
        && inner_base >= outer_base
        && inner_count <= outer_base + outer_count - inner_base;
}

[[nodiscard]] constexpr auto data_within(
    const myos_cap_attenuation& requested,
    const myos_cap_attenuation& ceiling) noexcept -> bool {
    switch (requested.kind) {
    case MYOS_OBJECT_KIND_THREAD:
    case MYOS_OBJECT_KIND_SCHED_CONTEXT:
    case MYOS_OBJECT_KIND_SCHED_DOMAIN:
    case MYOS_OBJECT_KIND_CSPACE:
    case MYOS_OBJECT_KIND_NOTIFICATION:
    case MYOS_OBJECT_KIND_VPROC:
    case MYOS_OBJECT_KIND_IRQ:
        return true;
    case MYOS_OBJECT_KIND_MEMORY:
    case MYOS_OBJECT_KIND_VSPACE:
        return range_within(
                   ceiling.words[0], ceiling.words[1], requested.words[0],
                   requested.words[1])
            && (requested.words[2] & ~ceiling.words[2]) == 0
            && (requested.words[3] & ~ceiling.words[3]) == 0;
    case MYOS_OBJECT_KIND_RESOURCE_POOL:
        return requested.words[0] <= ceiling.words[0]
            && requested.words[1] <= ceiling.words[1]
            && (requested.words[2] & ~ceiling.words[2]) == 0;
    case MYOS_OBJECT_KIND_ENDPOINT:
        return (requested.words[1] & ceiling.words[1]) == ceiling.words[1]
            && (requested.words[0] & ceiling.words[1]) == ceiling.words[0]
            && requested.words[2] <= ceiling.words[2];
    case MYOS_OBJECT_KIND_CHANNEL: {
        const bool ceiling_unbound =
            ceiling.words[1] == 0 && ceiling.words[2] == 0;
        const bool ceiling_exact =
            ceiling.words[1] != 0 && ceiling.words[2] == UINT64_MAX;
        const bool requested_unbound =
            requested.words[1] == 0 && requested.words[2] == 0;
        const bool requested_exact =
            requested.words[1] != 0 && requested.words[2] == UINT64_MAX;
        const bool same_side = requested.words[0] == ceiling.words[0];
        if (!same_side) {
            return false;
        }
        if (ceiling_unbound) {
            return requested_unbound;
        }
        return ceiling_exact && requested_exact
            && requested.words[1] == ceiling.words[1]
            && requested.words[2] == ceiling.words[2];
    }
    case MYOS_OBJECT_KIND_PAGER:
        return requested.words[0] <= ceiling.words[0];
    case MYOS_OBJECT_KIND_TUNNEL:
    case MYOS_OBJECT_KIND_INVALID:
    case MYOS_OBJECT_KIND_COUNT:
        return false;
    }
    return false;
}

[[nodiscard]] constexpr auto channel_mint_within(
    const myos_cap_attenuation& requested,
    const myos_cap_attenuation& ceiling) noexcept -> bool {
    return requested.kind == MYOS_OBJECT_KIND_CHANNEL
        && ceiling.kind == MYOS_OBJECT_KIND_CHANNEL
        && ceiling.words[0] == requested.words[0]
        && ceiling.words[1] == 0 && ceiling.words[2] == 0
        && requested.words[1] != 0 && requested.words[2] == UINT64_MAX;
}

/*
 * Compare one requested operation with a registered public ceiling.  The
 * mode distinction mirrors the three accepted import syscalls: Duplicate is
 * rights-only; TypedDelegate uses ordinary typed containment; ChannelMint is
 * the explicit unbound-to-exact same-side proof.
 */
[[nodiscard]] constexpr auto within(
    const myos_cap_attenuation& requested,
    const myos_cap_attenuation& ceiling,
    uint16_t mode) noexcept -> bool {
    if (!valid_descriptor(ceiling, DescriptorForm::Ceiling)
        || !rights_within(requested, ceiling)) {
        return false;
    }
    if (mode == MYOS_DEPLOY_IMPORT_DUPLICATE) {
        return valid_descriptor(requested, DescriptorForm::DuplicateRequest);
    }
    if (mode == MYOS_DEPLOY_IMPORT_CHANNEL_MINT) {
        return valid_descriptor(requested, DescriptorForm::TypedRequest)
            && channel_mint_within(requested, ceiling);
    }
    if (mode != MYOS_DEPLOY_IMPORT_TYPED_DELEGATE
        || !valid_descriptor(requested, DescriptorForm::TypedRequest)) {
        return false;
    }
    return data_within(requested, ceiling);
}

} // namespace myos::deploy::attenuation
