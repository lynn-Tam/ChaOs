#include <expected>
#include <optional>
#include <variant>
#include <cap/cap.hpp>
#include <uapi/endpoint.h>
#include <libk/checked_arithmetic.hpp>
#include <mm/types.hpp>
#include <uapi/capability.h>

namespace cap {

constexpr Rights memory_rights = Rights::of(
    Right::Duplicate,
    Right::Delegate,
    Right::Inspect,
    Right::Map,
    Right::Destroy,
    Right::Manage,
    Right::Revoke);

constexpr Rights vspace_rights = Rights::of(
    Right::Duplicate,
    Right::Delegate,
    Right::Inspect,
    Right::Reserve,
    Right::Map,
    Right::Unmap,
    Right::Protect,
    Right::Destroy,
    Right::Manage,
    Right::Revoke);

constexpr Rights resource_rights = Rights::of(
    Right::Duplicate,
    Right::Delegate,
    Right::Inspect,
    Right::Create,
    Right::Split,
    Right::Close,
    Right::Revoke);

constexpr Rights notification_rights = Rights::of(
    Right::Duplicate,
    Right::Delegate,
    Right::Inspect,
    Right::Signal,
    Right::Receive,
    Right::Destroy,
    Right::Revoke);

constexpr Rights endpoint_rights = Rights::of(
    Right::Duplicate,
    Right::Delegate,
    Right::Inspect,
    Right::Call,
    Right::Close,
    Right::Destroy,
    Right::Revoke);

constexpr Rights channel_rights = Rights::of(
    Right::Duplicate,
    Right::Delegate,
    Right::Inspect,
    Right::Send,
    Right::Receive,
    Right::Close,
    Right::Destroy,
    Right::Revoke);

constexpr Rights pager_rights = Rights::of(
    Right::Duplicate, Right::Delegate, Right::Inspect, Right::Attach,
    Right::Serve,
    Right::Supply, Right::Fail, Right::WritebackAck, Right::Close,
    Right::Destroy, Right::Revoke);

constexpr Rights irq_rights = Rights::of(
    Right::Duplicate, Right::Delegate, Right::Inspect, Right::Route,
    Right::Observe, Right::Ack, Right::Control, Right::Close, Right::Destroy,
    Right::Revoke);

[[nodiscard]] static auto valid(MemLimit limit) noexcept -> bool {
    return limit.range.limit().has_value()
        && mm::valid_perms(limit.perms);
}

[[nodiscard]] static auto valid(VmLimit limit) noexcept -> bool {
    return limit.range.valid()
        && !limit.range.empty()
        && (limit.range.base().raw() & (mm::page_size - 1)) == 0
        && (limit.range.size() & (mm::page_size - 1)) == 0
        && mm::valid_perms(limit.perms);
}

[[nodiscard]] static auto valid(Quota limit) noexcept -> bool {
    constexpr u64 valid_kinds =
        MYOS_OBJECT_KINDS;
    return (limit.object_kinds & ~valid_kinds) == 0;
}

[[nodiscard]] static auto valid(EpLimit limit) noexcept -> bool {
    return (limit.badge & ~limit.fixed) == 0
        && limit.cap_limit <= MYOS_ENDPOINT_MAX_CAPS;
}

[[nodiscard]] static auto valid(ChanLimit limit) noexcept -> bool {
    const bool side = limit.side == ChannelSide::A
        || limit.side == ChannelSide::B
        || limit.side == ChannelSide::Any;
    const bool unbound = limit.fixed == 0 && limit.badge == 0;
    const bool exact = limit.fixed == ~u64{} && limit.badge != 0;
    return side && (unbound || exact)
        && (limit.side != ChannelSide::Any || unbound);
}

[[nodiscard]] static auto valid(IrqRoute limit) noexcept -> bool {
    return limit.source != 0;
}

static auto valid(std::monostate) noexcept -> bool { return true; }
static auto valid(Badge p) noexcept -> bool { return p.badge != 0; }

static auto contains(std::monostate, std::monostate) noexcept -> bool { return true; }
static auto contains(MemLimit a, MemLimit b) noexcept -> bool {
    return a.range.contains(b.range) && a.perms.contains(b.perms);
}
static auto contains(VmLimit a, VmLimit b) noexcept -> bool {
    return a.range.contains(b.range)
        && a.perms.contains(b.perms);
}
static auto contains(Quota a, Quota b) noexcept -> bool {
    return a.budget.contains(b.budget) && (b.object_kinds & ~a.object_kinds) == 0;
}
static auto contains(Badge a, Badge b) noexcept -> bool { return a == b; }
static auto contains(IrqRoute a, IrqRoute b) noexcept -> bool { return a == b; }
static auto contains(EpLimit a, EpLimit b) noexcept -> bool {
    return (b.fixed & a.fixed) == a.fixed && (b.badge & a.fixed) == a.badge
        && b.cap_limit <= a.cap_limit;
}
static auto contains(ChanLimit a, ChanLimit b) noexcept -> bool {
    const bool root = a.side == ChannelSide::Any && a.unbound();
    return (root || a.side == b.side) && (!a.unbound() || b.unbound())
        && (!a.exact() || a == b) && (!root || b.side != ChannelSide::Any);
}

// One type/rights dispatch shared by validation and containment.
template<class F, class R>
static auto select(object::ObjectKind kind, F fn, R missing) noexcept -> R {
    using Kind = object::ObjectKind;
    switch (kind) {
    case Kind::IoSpace:
        return fn.template operator()<std::monostate>(Rights::of(
            Right::Duplicate, Right::Delegate, Right::Inspect, Right::Connect, Right::Close, Right::Revoke));
    case Kind::Device:
        return fn.template operator()<std::monostate>(Rights::of(
            Right::Duplicate, Right::Delegate, Right::Inspect, Right::Connect, Right::Revoke));
    case Kind::Thread:
        return fn.template operator()<std::monostate>(Rights::of(
            Right::Duplicate, Right::Delegate, Right::Inspect, Right::Control, Right::Destroy, Right::Observe, Right::Revoke));
    case Kind::Sc:
        return fn.template operator()<std::monostate>(Rights::of(
            Right::Duplicate, Right::Delegate, Right::Inspect, Right::Control, Right::Destroy, Right::Revoke));
    case Kind::Domain:
        return fn.template operator()<std::monostate>(Rights::of(
            Right::Duplicate, Right::Delegate, Right::Inspect, Right::Control, Right::Destroy, Right::Revoke));
    case Kind::CSpace:
        return fn.template operator()<std::monostate>(Rights::of(
            Right::Duplicate, Right::Delegate, Right::Inspect, Right::Manage, Right::Destroy, Right::Revoke));
    case Kind::Mem: return fn.template operator()<MemLimit>(memory_rights);
    case Kind::VSpace: return fn.template operator()<VmLimit>(vspace_rights);
    case Kind::group: return fn.template operator()<Quota>(resource_rights);
    case Kind::Notification: return fn.template operator()<Badge>(notification_rights);
    case Kind::Endpoint: return fn.template operator()<EpLimit>(endpoint_rights);
    case Kind::Channel: return fn.template operator()<ChanLimit>(channel_rights);
    case Kind::Pager: return fn.template operator()<std::monostate>(pager_rights);
    case Kind::Irq: return fn.template operator()<IrqRoute>(irq_rights);
    default: return missing;
    }
}

auto validate_ceiling(object::ObjectKind kind, View ceiling) noexcept -> bool {
    return select(kind, [&]<class T>(Rights allowed) noexcept {
        const auto* p = std::get_if<T>(&ceiling.data);
        return allowed.contains(ceiling.rights) && p && valid(*p);
    }, false);
}

auto compose(object::ObjectKind kind, View ceiling, View view) noexcept
    -> std::expected<View, PolicyError> {
    using Result = std::expected<View, PolicyError>;
    return select(kind, [&]<class T>(Rights allowed) noexcept -> Result {
        if (!allowed.contains(ceiling.rights) || !allowed.contains(view.rights))
            return std::unexpected(PolicyError::InvalidRights);
        const auto* a = std::get_if<T>(&ceiling.data);
        const auto* b = std::get_if<T>(&view.data);
        if (!a || !b || !valid(*a) || !valid(*b))
            return std::unexpected(PolicyError::InvalidData);
        if (!ceiling.rights.contains(view.rights) || !contains(*a, *b))
            return std::unexpected(PolicyError::Amplification);
        return (view);
    }, Result{std::unexpected(PolicyError::UnsupportedKind)});
}

auto attenuates(object::ObjectKind kind, View source, View child) noexcept -> bool {
    auto result = compose(kind, source, child);
    return result && result.value().rights == child.rights && result.value().data == child.data;
}

template<class T>
[[nodiscard]] static constexpr auto read_le(const byte* p) noexcept -> T {
    T n{};
    for (usize i = 0; i < sizeof(T); ++i)
        n |= static_cast<T>(p[i]) << (i * 8);
    return n;
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

[[nodiscard]] auto u8_value(u64 raw) noexcept -> std::optional<u8> {
    return raw <= std::numeric_limits<u8>::max()
        ? std::optional<u8>{static_cast<u8>(raw)}
        : std::nullopt;
}

[[nodiscard]] auto usize_value(u64 raw) noexcept -> std::optional<usize> {
    return raw <= std::numeric_limits<usize>::max()
        ? std::optional<usize>{static_cast<usize>(raw)}
        : std::nullopt;
}

[[nodiscard]] auto decode_access(u64 raw) noexcept
    -> std::optional<mm::Perms> {
    const auto value = u8_value(raw);
    if (!value) {
        return std::nullopt;
    }
    const auto access = mm::Perms::from_raw(*value);
    return mm::valid_perms(access)
        ? std::optional<mm::Perms>{access}
        : std::nullopt;
}

[[nodiscard]] auto rights_value(u64 raw) noexcept
    -> std::expected<Rights, AttenuationError> {
    const auto rights = Rights::parse(raw, MYOS_RIGHT_MASK);
    return rights
        ? std::expected<Rights, AttenuationError>{(*rights)}
        : std::expected<Rights, AttenuationError>{
              std::unexpected(AttenuationError::InvalidRights)};
}

auto decode_attenuation(libk::Span<const byte> bytes) noexcept
    -> std::expected<Attenuation, AttenuationError> {
    if (bytes.size() != MYOS_CAP_ATTENUATION_SIZE) {
        return std::unexpected(AttenuationError::InvalidSize);
    }
    Attenuation descriptor{
        .version = read_le<u16>(bytes.data() + MYOS_CAP_ATTENUATION_VERSION_OFFSET),
        .kind = read_le<u16>(bytes.data() + MYOS_CAP_ATTENUATION_KIND_OFFSET),
        .size = read_le<u32>(bytes.data() + MYOS_CAP_ATTENUATION_SIZE_OFFSET),
        .rights = read_le<u64>(bytes.data() + MYOS_CAP_ATTENUATION_RIGHTS_OFFSET),
    };
    for (usize index = 0; index < 6; ++index) {
        descriptor.words[index] = read_le<u64>(
            bytes.data() + MYOS_CAP_ATTENUATION_WORD0_OFFSET
            + index * sizeof(u64));
    }
    if (descriptor.version != MYOS_CAP_ATTENUATION_VERSION_CURRENT) {
        return std::unexpected(AttenuationError::InvalidVersion);
    }
    if (descriptor.size != MYOS_CAP_ATTENUATION_SIZE) {
        return std::unexpected(AttenuationError::InvalidSize);
    }
    if (!attenuation_kind(descriptor.kind)) {
        return std::unexpected(AttenuationError::InvalidKind);
    }
    if (!Rights::parse(descriptor.rights, MYOS_RIGHT_MASK)) {
        return std::unexpected(AttenuationError::InvalidRights);
    }
    return (descriptor);
}

auto attenuation_kind(u16 raw) noexcept
    -> std::optional<object::ObjectKind> {
    switch (raw) {
    case MYOS_OBJECT_KIND_IO_SPACE:
        return object::ObjectKind::IoSpace;
    case MYOS_OBJECT_KIND_DEVICE:
        return object::ObjectKind::Device;
    case MYOS_OBJECT_KIND_THREAD:
        return object::ObjectKind::Thread;
    case MYOS_OBJECT_KIND_SCHED_CONTEXT:
        return object::ObjectKind::Sc;
    case MYOS_OBJECT_KIND_SCHED_DOMAIN:
        return object::ObjectKind::Domain;
    case MYOS_OBJECT_KIND_CSPACE:
        return object::ObjectKind::CSpace;
    case MYOS_OBJECT_KIND_MEMORY:
        return object::ObjectKind::Mem;
    case MYOS_OBJECT_KIND_VSPACE:
        return object::ObjectKind::VSpace;
    case MYOS_OBJECT_KIND_RESOURCE_POOL:
        return object::ObjectKind::group;
    case MYOS_OBJECT_KIND_NOTIFICATION:
        return object::ObjectKind::Notification;
    case MYOS_OBJECT_KIND_ENDPOINT:
        return object::ObjectKind::Endpoint;
    case MYOS_OBJECT_KIND_CHANNEL:
        return object::ObjectKind::Channel;
    case MYOS_OBJECT_KIND_PAGER:
        return object::ObjectKind::Pager;
    case MYOS_OBJECT_KIND_IRQ:
        return object::ObjectKind::Irq;
    case MYOS_OBJECT_KIND_INVALID:
    case MYOS_OBJECT_KIND_COUNT:
        return std::nullopt;
    }
    return std::nullopt;
}

auto make_attenuation_ceiling(
    object::ObjectKind kind,
    const View& source,
    const Attenuation& descriptor) noexcept
    -> std::expected<View, AttenuationError> {
    const auto source_kind = attenuation_kind(descriptor.kind);
    if (!source_kind || *source_kind != kind) {
        return std::unexpected(AttenuationError::InvalidKind);
    }
    const auto rights = rights_value(descriptor.rights);
    if (!rights) {
        return std::unexpected(rights.error());
    }
    const Rights child_rights = rights.value();

    switch (kind) {
    case object::ObjectKind::IoSpace:
    case object::ObjectKind::Device:
    case object::ObjectKind::Thread:
    case object::ObjectKind::Sc:
    case object::ObjectKind::Domain:
    case object::ObjectKind::CSpace:
    case object::ObjectKind::Pager:
        if (!words_zero(descriptor, 0)) {
            return std::unexpected(AttenuationError::InvalidWord);
        }
        return (View{child_rights, std::monostate{}});

    case object::ObjectKind::Mem: {
        if (!words_zero(descriptor, 3)) {
            return std::unexpected(AttenuationError::InvalidWord);
        }
        const auto first = usize_value(descriptor.words[0]);
        const auto count = usize_value(descriptor.words[1]);
        const auto access = decode_access(descriptor.words[2]);
        const mm::ObjectRange range{
            first ? *first : 0, count ? *count : 0};
        if (!first || !count || !access
            || *count == 0
            || !range.limit()) {
            return std::unexpected(AttenuationError::InvalidRange);
        }
        return (View{
            child_rights,
            MemLimit{range, *access}});
    }

    case object::ObjectKind::VSpace: {
        if (!words_zero(descriptor, 3)) {
            return std::unexpected(AttenuationError::InvalidWord);
        }
        const auto base = usize_value(descriptor.words[0]);
        const auto size = usize_value(descriptor.words[1]);
        const auto access = decode_access(descriptor.words[2]);
        const auto* const upper = std::get_if<VmLimit>(&source.data);
        if (!base || !size || !access || *base == 0 || *size == 0
            || (*base % mm::page_size) != 0
            || (*size % mm::page_size) != 0
            || upper == nullptr) {
            return std::unexpected(AttenuationError::InvalidRange);
        }
        const mm::VRange range{
            mm::Virt{*base}, *size};
        if (!range.valid() || range.empty()) {
            return std::unexpected(AttenuationError::InvalidRange);
        }
        return (View{
            child_rights,
            VmLimit{range, *access}});
    }

    case object::ObjectKind::group: {
        if (!words_zero(descriptor, 3)) {
            return std::unexpected(AttenuationError::InvalidWord);
        }
        return (View{
            child_rights,
            Quota{
                resource::budget{
                    descriptor.words[0], descriptor.words[1]},
                descriptor.words[2]}});
    }

    case object::ObjectKind::Notification: {
        if (!words_zero(descriptor, 0)) {
            return std::unexpected(AttenuationError::InvalidWord);
        }
        const auto* const upper =
            std::get_if<Badge>(&source.data);
        return upper == nullptr
            ? std::expected<View, AttenuationError>{
                  std::unexpected(AttenuationError::InvalidData)}
            : std::expected<View, AttenuationError>{(
                  View{child_rights, *upper})};
    }

    case object::ObjectKind::Irq: {
        if (!words_zero(descriptor, 0)) {
            return std::unexpected(AttenuationError::InvalidWord);
        }
        const auto* const upper = std::get_if<IrqRoute>(&source.data);
        return upper == nullptr
            ? std::expected<View, AttenuationError>{
                  std::unexpected(AttenuationError::InvalidData)}
            : std::expected<View, AttenuationError>{(
                  View{child_rights, *upper})};
    }

    case object::ObjectKind::Endpoint: {
        if (!words_zero(descriptor, 3)
            || (descriptor.words[0] & ~descriptor.words[1]) != 0
            || descriptor.words[2] > MYOS_ENDPOINT_MAX_CAPS) {
            return std::unexpected(AttenuationError::InvalidData);
        }
        const auto cap_limit = usize_value(descriptor.words[2]);
        if (!cap_limit) {
            return std::unexpected(AttenuationError::InvalidData);
        }
        return (View{
            child_rights,
            EpLimit{
                descriptor.words[0], descriptor.words[1],
                *cap_limit}});
    }

    case object::ObjectKind::Channel: {
        if (!words_zero(descriptor, 3)
            || (descriptor.words[0] != MYOS_CAP_CHANNEL_SIDE_A
                && descriptor.words[0] != MYOS_CAP_CHANNEL_SIDE_B)) {
            return std::unexpected(AttenuationError::InvalidData);
        }
        const bool unbound = descriptor.words[1] == 0
            && descriptor.words[2] == 0;
        const bool exact = descriptor.words[1] != 0
            && descriptor.words[2] == ~u64{};
        if (!unbound && !exact) {
            return std::unexpected(AttenuationError::InvalidData);
        }
        const auto side = descriptor.words[0] == MYOS_CAP_CHANNEL_SIDE_A
            ? ChannelSide::A : ChannelSide::B;
        return (View{
            child_rights,
            ChanLimit{side, descriptor.words[1], descriptor.words[2]}});
    }


    case object::ObjectKind::Invalid:
    case object::ObjectKind::Count:
        return std::unexpected(AttenuationError::UnsupportedKind);
    }
    return std::unexpected(AttenuationError::UnsupportedKind);
}

} // namespace cap
