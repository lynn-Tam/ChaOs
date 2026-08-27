#include <cap/attenuation.hpp>

#include <libk/checked_arithmetic.hpp>
#include <mm/addr.hpp>
#include <uapi/capability.h>
#include <uapi/endpoint.h>

namespace kernel::cap {
namespace {

[[nodiscard]] constexpr auto read16(const byte* bytes) noexcept -> u16 {
    return static_cast<u16>(bytes[0])
        | static_cast<u16>(static_cast<u16>(bytes[1]) << 8);
}

[[nodiscard]] constexpr auto read32(const byte* bytes) noexcept -> u32 {
    return static_cast<u32>(bytes[0])
        | (static_cast<u32>(bytes[1]) << 8)
        | (static_cast<u32>(bytes[2]) << 16)
        | (static_cast<u32>(bytes[3]) << 24);
}

[[nodiscard]] constexpr auto read64(const byte* bytes) noexcept -> u64 {
    u64 result{};
    for (usize index = 0; index < sizeof(u64); ++index) {
        result |= static_cast<u64>(bytes[index]) << (index * 8);
    }
    return result;
}

[[nodiscard]] constexpr auto words_zero(
    const Attenuation& descriptor,
    usize first_nonzero) noexcept -> bool {
    for (usize index = first_nonzero; index < 6; ++index) {
        if (descriptor.words[index] != 0) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] auto u8_value(u64 raw) noexcept -> libk::optional<u8> {
    return raw <= libk::numeric_limits<u8>::max()
        ? libk::optional<u8>{static_cast<u8>(raw)}
        : libk::nullopt;
}

[[nodiscard]] auto usize_value(u64 raw) noexcept -> libk::optional<usize> {
    return raw <= libk::numeric_limits<usize>::max()
        ? libk::optional<usize>{static_cast<usize>(raw)}
        : libk::nullopt;
}

[[nodiscard]] auto decode_access(u64 raw) noexcept
    -> libk::optional<kernel::mm::AccessMask> {
    const auto value = u8_value(raw);
    if (!value) {
        return libk::nullopt;
    }
    const auto access = kernel::mm::AccessMask::from_raw(*value);
    return kernel::mm::valid_access(access)
        ? libk::optional<kernel::mm::AccessMask>{access}
        : libk::nullopt;
}

[[nodiscard]] auto decode_types(u64 raw) noexcept
    -> libk::optional<kernel::mm::MemoryTypes> {
    const auto value = u8_value(raw);
    if (!value) {
        return libk::nullopt;
    }
    const auto types = kernel::mm::MemoryTypes::from_raw(*value);
    return kernel::mm::valid_memory_types(types)
        ? libk::optional<kernel::mm::MemoryTypes>{types}
        : libk::nullopt;
}

[[nodiscard]] auto rights_value(u64 raw) noexcept
    -> libk::Expected<Rights, AttenuationError> {
    const auto rights = Rights::from_raw(raw);
    return rights
        ? libk::Expected<Rights, AttenuationError>{libk::expected(*rights)}
        : libk::Expected<Rights, AttenuationError>{
              libk::unexpected(AttenuationError::InvalidRights)};
}

} // namespace

auto decode_attenuation(libk::Span<const byte> bytes) noexcept
    -> libk::Expected<Attenuation, AttenuationError> {
    if (bytes.size() != MYOS_CAP_ATTENUATION_SIZE) {
        return libk::unexpected(AttenuationError::InvalidSize);
    }
    Attenuation descriptor{
        .version = read16(bytes.data() + MYOS_CAP_ATTENUATION_VERSION_OFFSET),
        .kind = read16(bytes.data() + MYOS_CAP_ATTENUATION_KIND_OFFSET),
        .size = read32(bytes.data() + MYOS_CAP_ATTENUATION_SIZE_OFFSET),
        .rights = read64(bytes.data() + MYOS_CAP_ATTENUATION_RIGHTS_OFFSET),
    };
    for (usize index = 0; index < 6; ++index) {
        descriptor.words[index] = read64(
            bytes.data() + MYOS_CAP_ATTENUATION_WORD0_OFFSET
            + index * sizeof(u64));
    }
    if (descriptor.version != MYOS_CAP_ATTENUATION_VERSION_CURRENT) {
        return libk::unexpected(AttenuationError::InvalidVersion);
    }
    if (descriptor.size != MYOS_CAP_ATTENUATION_SIZE) {
        return libk::unexpected(AttenuationError::InvalidSize);
    }
    if (!attenuation_kind(descriptor.kind)) {
        return libk::unexpected(AttenuationError::InvalidKind);
    }
    if (!Rights::from_raw(descriptor.rights)) {
        return libk::unexpected(AttenuationError::InvalidRights);
    }
    return libk::expected(descriptor);
}

auto attenuation_kind(u16 raw) noexcept
    -> libk::optional<object::ObjectKind> {
    switch (raw) {
    case MYOS_OBJECT_KIND_THREAD:
        return object::ObjectKind::Thread;
    case MYOS_OBJECT_KIND_SCHED_CONTEXT:
        return object::ObjectKind::SchedulingContext;
    case MYOS_OBJECT_KIND_SCHED_DOMAIN:
        return object::ObjectKind::SchedulingDomain;
    case MYOS_OBJECT_KIND_CSPACE:
        return object::ObjectKind::CSpace;
    case MYOS_OBJECT_KIND_MEMORY:
        return object::ObjectKind::MemoryObject;
    case MYOS_OBJECT_KIND_VSPACE:
        return object::ObjectKind::VSpace;
    case MYOS_OBJECT_KIND_RESOURCE_POOL:
        return object::ObjectKind::ResourcePool;
    case MYOS_OBJECT_KIND_NOTIFICATION:
        return object::ObjectKind::Notification;
    case MYOS_OBJECT_KIND_VPROC:
        return object::ObjectKind::Vproc;
    case MYOS_OBJECT_KIND_ENDPOINT:
        return object::ObjectKind::Endpoint;
    case MYOS_OBJECT_KIND_CHANNEL:
        return object::ObjectKind::Channel;
    case MYOS_OBJECT_KIND_PAGER:
        return object::ObjectKind::Pager;
    case MYOS_OBJECT_KIND_IRQ:
        return object::ObjectKind::Irq;
    case MYOS_OBJECT_KIND_TUNNEL:
    case MYOS_OBJECT_KIND_INVALID:
    case MYOS_OBJECT_KIND_COUNT:
        return libk::nullopt;
    }
    return libk::nullopt;
}

auto make_attenuation_ceiling(
    object::ObjectKind kind,
    const EffectiveAuthority& source,
    const Attenuation& descriptor) noexcept
    -> libk::Expected<GrantCeiling, AttenuationError> {
    const auto source_kind = attenuation_kind(descriptor.kind);
    if (!source_kind || *source_kind != kind) {
        return libk::unexpected(AttenuationError::InvalidKind);
    }
    const auto rights = rights_value(descriptor.rights);
    if (!rights) {
        return libk::unexpected(rights.error());
    }
    const Rights child_rights = rights.value();

    switch (kind) {
    case object::ObjectKind::Thread:
    case object::ObjectKind::Vproc:
    case object::ObjectKind::SchedulingContext:
    case object::ObjectKind::SchedulingDomain:
    case object::ObjectKind::CSpace:
        if (!words_zero(descriptor, 0)) {
            return libk::unexpected(AttenuationError::InvalidWord);
        }
        return libk::expected(GrantCeiling{child_rights, libk::monostate{}});

    case object::ObjectKind::MemoryObject: {
        if (!words_zero(descriptor, 4)) {
            return libk::unexpected(AttenuationError::InvalidWord);
        }
        const auto first = usize_value(descriptor.words[0]);
        const auto count = usize_value(descriptor.words[1]);
        const auto access = decode_access(descriptor.words[2]);
        const auto types = decode_types(descriptor.words[3]);
        const kernel::mm::ObjectRange range{
            first ? *first : 0, count ? *count : 0};
        if (!first || !count || !access || !types
            || *count == 0
            || !range.end()) {
            return libk::unexpected(AttenuationError::InvalidRange);
        }
        return libk::expected(GrantCeiling{
            child_rights,
            MemoryAuthority{range, *access, *types}});
    }

    case object::ObjectKind::VSpace: {
        if (!words_zero(descriptor, 4)) {
            return libk::unexpected(AttenuationError::InvalidWord);
        }
        const auto base = usize_value(descriptor.words[0]);
        const auto size = usize_value(descriptor.words[1]);
        const auto access = decode_access(descriptor.words[2]);
        const auto types = decode_types(descriptor.words[3]);
        const auto* const upper = libk::get_if<VSpaceAuthority>(&source.data);
        if (!base || !size || !access || !types || *base == 0 || *size == 0
            || (*base % kernel::mm::page_size) != 0
            || (*size % kernel::mm::page_size) != 0
            || upper == nullptr) {
            return libk::unexpected(AttenuationError::InvalidRange);
        }
        const kernel::mm::VirtRange range{
            kernel::mm::VirtAddr{*base}, *size};
        if (!range.valid() || range.empty()) {
            return libk::unexpected(AttenuationError::InvalidRange);
        }
        return libk::expected(GrantCeiling{
            child_rights,
            VSpaceAuthority{upper->region, range, *access, *types}});
    }

    case object::ObjectKind::ResourcePool: {
        if (!words_zero(descriptor, 3)) {
            return libk::unexpected(AttenuationError::InvalidWord);
        }
        return libk::expected(GrantCeiling{
            child_rights,
            ResourcePoolAuthority{
                kernel::resource::Budget{
                    descriptor.words[0], descriptor.words[1]},
                descriptor.words[2]}});
    }

    case object::ObjectKind::Notification: {
        if (!words_zero(descriptor, 0)) {
            return libk::unexpected(AttenuationError::InvalidWord);
        }
        const auto* const upper =
            libk::get_if<NotificationAuthority>(&source.data);
        return upper == nullptr
            ? libk::Expected<GrantCeiling, AttenuationError>{
                  libk::unexpected(AttenuationError::InvalidData)}
            : libk::Expected<GrantCeiling, AttenuationError>{libk::expected(
                  GrantCeiling{child_rights, *upper})};
    }

    case object::ObjectKind::Irq: {
        if (!words_zero(descriptor, 0)) {
            return libk::unexpected(AttenuationError::InvalidWord);
        }
        const auto* const upper = libk::get_if<IrqAuthority>(&source.data);
        return upper == nullptr
            ? libk::Expected<GrantCeiling, AttenuationError>{
                  libk::unexpected(AttenuationError::InvalidData)}
            : libk::Expected<GrantCeiling, AttenuationError>{libk::expected(
                  GrantCeiling{child_rights, *upper})};
    }

    case object::ObjectKind::Endpoint: {
        if (!words_zero(descriptor, 3)
            || (descriptor.words[0] & ~descriptor.words[1]) != 0
            || descriptor.words[2] > MYOS_ENDPOINT_MAX_CAPS) {
            return libk::unexpected(AttenuationError::InvalidData);
        }
        const auto cap_limit = usize_value(descriptor.words[2]);
        if (!cap_limit) {
            return libk::unexpected(AttenuationError::InvalidData);
        }
        return libk::expected(GrantCeiling{
            child_rights,
            EndpointAuthority{
                descriptor.words[0], descriptor.words[1],
                *cap_limit}});
    }

    case object::ObjectKind::Channel: {
        if (!words_zero(descriptor, 3)
            || (descriptor.words[0] != MYOS_CAP_CHANNEL_SIDE_A
                && descriptor.words[0] != MYOS_CAP_CHANNEL_SIDE_B)) {
            return libk::unexpected(AttenuationError::InvalidData);
        }
        const bool unbound = descriptor.words[1] == 0
            && descriptor.words[2] == 0;
        const bool exact = descriptor.words[1] != 0
            && descriptor.words[2] == ~u64{};
        if (!unbound && !exact) {
            return libk::unexpected(AttenuationError::InvalidData);
        }
        const auto side = descriptor.words[0] == MYOS_CAP_CHANNEL_SIDE_A
            ? ChannelSide::A : ChannelSide::B;
        return libk::expected(GrantCeiling{
            child_rights,
            ChannelAuthority{side, descriptor.words[1], descriptor.words[2]}});
    }

    case object::ObjectKind::Pager: {
        if (!words_zero(descriptor, 1)) {
            return libk::unexpected(AttenuationError::InvalidWord);
        }
        const auto max_pages = usize_value(descriptor.words[0]);
        const auto* const upper = libk::get_if<PagerAuthority>(&source.data);
        if (!max_pages || *max_pages == 0 || upper == nullptr
            || upper->backing_key == 0) {
            return libk::unexpected(AttenuationError::InvalidData);
        }
        return libk::expected(GrantCeiling{
            child_rights, PagerAuthority{upper->backing_key, *max_pages}});
    }

    case object::ObjectKind::Tunnel:
    case object::ObjectKind::Invalid:
    case object::ObjectKind::Count:
        return libk::unexpected(AttenuationError::UnsupportedKind);
    }
    return libk::unexpected(AttenuationError::UnsupportedKind);
}

} // namespace kernel::cap
