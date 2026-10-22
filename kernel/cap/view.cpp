#include <expected>
#include <optional>
#include <variant>
#include <cap/cap.hpp>
#include <uapi/ipc.h>
#include <libk/checked_arithmetic.hpp>
#include <mm/types.hpp>
#include <uapi/cap.h>

namespace cap {

auto describe(ObjKind kind, View view) noexcept -> CapView {
    CapView result{.version = CAP_ATTENUATION_VERSION_CURRENT, .kind = kind,
        .size = sizeof(CapView), .rights = view.rights.raw(), .words = {}};
    std::visit([&]<class T>(const T& limit) noexcept {
        auto& w = result.words;
        if constexpr (std::is_same_v<T, MemLimit> || std::is_same_v<T, VmLimit> || std::is_same_v<T, PhysLimit>) {
            if constexpr (std::is_same_v<T, MemLimit>) w[0] = limit.range.base();
            else w[0] = limit.range.base().raw();
            w[1] = limit.range.size();
            if constexpr (!std::is_same_v<T, PhysLimit>) w[2] = limit.perms.raw();
        } else if constexpr (std::is_same_v<T, QuotaLimit>) {
            w[0] = limit.budget.memory; w[1] = limit.budget.caps; w[2] = limit.object_kinds;
        } else if constexpr (std::is_same_v<T, ScopeLimit>) {
            w[0] = limit.kinds; w[1] = limit.facets; w[2] = limit.depth;
        } else if constexpr (std::is_same_v<T, IndexLimit>) {
            w[0] = limit.first; w[1] = limit.count;
        } else if constexpr (std::is_same_v<T, Badge>) w[0] = limit.badge;
        else if constexpr (std::is_same_v<T, ChanLimit>) {
            w[0] = static_cast<u64>(limit.side); w[1] = limit.badge; w[2] = limit.fixed;
        } else if constexpr (std::is_same_v<T, IrqRoute>) { w[0] = limit.source; w[1] = limit.level; }
    }, view.data);
    return result;
}

constexpr Rights memory_rights = Rights::of(
    Right::Duplicate,
    Right::Delegate,
    Right::Inspect,
    Right::Map,
    Right::Destroy,
    Right::Manage,
    Right::Adopt, Right::Reassign, Right::Revoke);

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
    Right::Adopt, Right::Reassign, Right::Revoke);

constexpr Rights resource_rights = Rights::of(
    Right::Duplicate,
    Right::Delegate,
    Right::Inspect,
    Right::Create,
    Right::Control, Right::Split,
    Right::Close, Right::Destroy,
    Right::Adopt, Right::Reassign, Right::Revoke);

constexpr Rights scope_rights = Rights::of(Right::Duplicate, Right::Delegate, Right::Inspect,
    Right::Create, Right::Manage, Right::Close, Right::Destroy, Right::Adopt, Right::Reassign, Right::Revoke);

constexpr Rights notification_rights = Rights::of(
    Right::Duplicate,
    Right::Delegate,
    Right::Inspect,
    Right::Signal,
    Right::Receive,
    Right::Destroy,
    Right::Adopt, Right::Reassign, Right::Revoke);

constexpr Rights channel_rights = Rights::of(
    Right::Duplicate,
    Right::Delegate,
    Right::Inspect,
    Right::Send,
    Right::Receive,
    Right::Close,
    Right::Destroy,
    Right::Adopt, Right::Reassign, Right::Revoke);

constexpr Rights pager_rights = Rights::of(
    Right::Duplicate, Right::Delegate, Right::Inspect, Right::Attach,
    Right::Serve,
    Right::Supply, Right::Fail, Right::WritebackAck, Right::Close,
    Right::Destroy, Right::Adopt, Right::Reassign, Right::Revoke);

constexpr Rights irq_rights = Rights::of(
    Right::Duplicate, Right::Delegate, Right::Inspect, Right::Route,
    Right::Observe, Right::Ack, Right::Control, Right::Close, Right::Destroy,
    Right::Adopt, Right::Reassign, Right::Revoke);

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

[[nodiscard]] static auto valid(QuotaLimit limit) noexcept -> bool {
    constexpr u64 valid_kinds =
        OBJ_KINDS;
    return (limit.object_kinds & ~valid_kinds) == 0;
}

static auto valid(ScopeLimit limit) noexcept -> bool {
    return !(limit.kinds & ~OBJ_KINDS) && !(limit.facets & ~SCOPE_FACETS) && limit.depth <= 64;
}
static auto contains(ScopeLimit a, ScopeLimit b) noexcept -> bool {
    return !(b.kinds & ~a.kinds) && !(b.facets & ~a.facets) && b.depth <= a.depth;
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

static auto valid(PhysLimit p) noexcept -> bool { return p.range.valid(); }
static auto contains(PhysLimit a, PhysLimit b) noexcept -> bool { return a.range.contains(b.range); }

static auto valid(IndexLimit p) noexcept -> bool {
    return p.count && p.count <= SIZE_MAX - p.first;
}
static auto contains(IndexLimit a, IndexLimit b) noexcept -> bool {
    return b.first >= a.first && b.first - a.first < a.count
        && b.count <= a.count - (b.first - a.first);
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
static auto contains(QuotaLimit a, QuotaLimit b) noexcept -> bool {
    return a.budget.contains(b.budget) && (b.object_kinds & ~a.object_kinds) == 0;
}
static auto contains(Badge a, Badge b) noexcept -> bool { return a == b; }
static auto contains(IrqRoute a, IrqRoute b) noexcept -> bool { return a == b; }
static auto contains(ChanLimit a, ChanLimit b) noexcept -> bool {
    const bool root = a.side == ChannelSide::Any && a.unbound();
    return (root || a.side == b.side) && (!a.unbound() || b.unbound())
        && (!a.exact() || a == b) && (!root || b.side != ChannelSide::Any);
}

// One type/rights dispatch shared by validation and containment.
template<class F, class R>
static auto select(ObjKind kind, F fn, R missing) noexcept -> R {
    switch (kind) {
    case ObjPhys:
        return fn.template operator()<PhysLimit>(Rights::of(
            Right::Duplicate, Right::Delegate, Right::Inspect, Right::Create, Right::Control, Right::Adopt, Right::Reassign, Right::Revoke));
    case ObjIoSpace:
        return fn.template operator()<std::monostate>(Rights::of(
            Right::Duplicate, Right::Delegate, Right::Inspect, Right::Map, Right::Unmap, Right::Observe, Right::Close, Right::Destroy, Right::Adopt, Right::Reassign, Right::Revoke));
    case ObjIrqCtl:
        return fn.template operator()<IndexLimit>(Rights::of(
            Right::Duplicate, Right::Delegate, Right::Inspect, Right::Create, Right::Destroy, Right::Adopt, Right::Reassign, Right::Revoke));
    case ObjIommu:
        return fn.template operator()<IndexLimit>(Rights::of(
            Right::Duplicate, Right::Delegate, Right::Inspect, Right::Create, Right::Destroy, Right::Adopt, Right::Reassign, Right::Revoke));
    case ObjThread:
        return fn.template operator()<std::monostate>(Rights::of(
            Right::Duplicate, Right::Delegate, Right::Inspect, Right::Control, Right::Destroy, Right::Observe, Right::Adopt, Right::Reassign, Right::Revoke));
    case ObjSc:
        return fn.template operator()<std::monostate>(Rights::of(
            Right::Duplicate, Right::Delegate, Right::Inspect, Right::Control, Right::Attach, Right::Destroy, Right::Adopt, Right::Reassign, Right::Revoke));
    case ObjDomain:
        return fn.template operator()<std::monostate>(Rights::of(
            Right::Duplicate, Right::Delegate, Right::Inspect, Right::Create, Right::Close, Right::Control, Right::Destroy, Right::Adopt, Right::Reassign, Right::Revoke));
    case ObjCSpace:
        return fn.template operator()<std::monostate>(Rights::of(
            Right::Duplicate, Right::Delegate, Right::Inspect, Right::Manage, Right::Destroy, Right::Adopt, Right::Reassign, Right::Revoke));
    case ObjMem: return fn.template operator()<MemLimit>(memory_rights);
    case ObjVSpace: return fn.template operator()<VmLimit>(vspace_rights);
    case ObjQuota: return fn.template operator()<QuotaLimit>(resource_rights);
    case ObjScope: return fn.template operator()<ScopeLimit>(scope_rights);
    case ObjNotification: return fn.template operator()<Badge>(notification_rights);
    case ObjChannel: return fn.template operator()<ChanLimit>(channel_rights);
    case ObjPager: return fn.template operator()<std::monostate>(pager_rights);
    case ObjIoOp:
    case ObjVmOp: return fn.template operator()<std::monostate>(Rights::of(
        Right::Duplicate, Right::Delegate, Right::Inspect, Right::Control, Right::Observe,
        Right::Destroy, Right::Adopt, Right::Reassign, Right::Revoke));
    case ObjPort: return fn.template operator()<std::monostate>(Rights::of(
        Right::Duplicate, Right::Delegate, Right::Inspect, Right::Receive, Right::Manage,
        Right::Destroy, Right::Adopt, Right::Reassign, Right::Revoke));
    case ObjIrq: return fn.template operator()<IrqRoute>(irq_rights);
    default: return missing;
    }
}

auto validate_ceiling(ObjKind kind, View ceiling) noexcept -> bool {
    return select(kind, [&]<class T>(Rights allowed) noexcept {
        const auto* p = std::get_if<T>(&ceiling.data);
        return allowed.contains(ceiling.rights) && p && valid(*p);
    }, false);
}

auto compose(ObjKind kind, View ceiling, View view) noexcept
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

auto attenuates(ObjKind kind, View source, View child) noexcept -> bool {
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
    const auto rights = Rights::parse(raw, RIGHT_MASK);
    return rights
        ? std::expected<Rights, AttenuationError>{(*rights)}
        : std::expected<Rights, AttenuationError>{
              std::unexpected(AttenuationError::InvalidRights)};
}

auto decode_attenuation(std::span<const byte> bytes) noexcept
    -> std::expected<Attenuation, AttenuationError> {
    if (bytes.size() != CAP_ATTENUATION_SIZE) {
        return std::unexpected(AttenuationError::InvalidSize);
    }
    Attenuation descriptor{
        .version = read_le<u16>(bytes.data() + CAP_ATTENUATION_VERSION_OFFSET),
        .kind = read_le<u16>(bytes.data() + CAP_ATTENUATION_KIND_OFFSET),
        .size = read_le<u32>(bytes.data() + CAP_ATTENUATION_SIZE_OFFSET),
        .rights = read_le<u64>(bytes.data() + CAP_ATTENUATION_RIGHTS_OFFSET),
    };
    for (usize index = 0; index < 6; ++index) {
        descriptor.words[index] = read_le<u64>(
            bytes.data() + CAP_ATTENUATION_WORD0_OFFSET
            + index * sizeof(u64));
    }
    if (descriptor.version != CAP_ATTENUATION_VERSION_CURRENT) {
        return std::unexpected(AttenuationError::InvalidVersion);
    }
    if (descriptor.size != CAP_ATTENUATION_SIZE) {
        return std::unexpected(AttenuationError::InvalidSize);
    }
    if (!attenuation_kind(descriptor.kind)) {
        return std::unexpected(AttenuationError::InvalidKind);
    }
    if (!Rights::parse(descriptor.rights, RIGHT_MASK)) {
        return std::unexpected(AttenuationError::InvalidRights);
    }
    return (descriptor);
}

auto attenuation_kind(u16 raw) noexcept
    -> std::optional<ObjKind> {
    if (raw >= ObjCount || !(OBJ_KINDS & (u64{1} << raw))) return std::nullopt;
    return static_cast<ObjKind>(raw);
}

auto make_attenuation_ceiling(
    ObjKind kind,
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
    case ObjIoSpace:
    case ObjThread:
    case ObjSc:
    case ObjDomain:
    case ObjCSpace:
    case ObjPager:
    case ObjVmOp:
    case ObjIoOp:
    case ObjPort:
        if (!words_zero(descriptor, 0)) {
            return std::unexpected(AttenuationError::InvalidWord);
        }
        return (View{child_rights, std::monostate{}});

    case ObjPhys: {
        const PhysLimit limit{{mm::Page{descriptor.words[0]}, descriptor.words[1]}};
        if (!words_zero(descriptor, 2) || !valid(limit))
            return std::unexpected(AttenuationError::InvalidRange);
        return View{child_rights, limit};
    }
    case ObjIrqCtl:
    case ObjIommu: {
        const IndexLimit limit{descriptor.words[0], descriptor.words[1]};
        if (!words_zero(descriptor, 2) || !valid(limit))
            return std::unexpected(AttenuationError::InvalidRange);
        return View{child_rights, limit};
    }
    case ObjMem: {
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

    case ObjVSpace: {
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
            mm::va_t{*base}, *size};
        if (!range.valid() || range.empty()) {
            return std::unexpected(AttenuationError::InvalidRange);
        }
        return (View{
            child_rights,
            VmLimit{range, *access}});
    }

    case ObjScope: {
        if (!words_zero(descriptor, 3) || descriptor.words[1] > UINT32_MAX || descriptor.words[2] > 64)
            return std::unexpected(AttenuationError::InvalidWord);
        return View{child_rights, ScopeLimit{descriptor.words[0], static_cast<u32>(descriptor.words[1]),
            static_cast<u32>(descriptor.words[2])}};
    }
    case ObjQuota: {
        if (!words_zero(descriptor, 3)) {
            return std::unexpected(AttenuationError::InvalidWord);
        }
        return (View{
            child_rights,
            QuotaLimit{
                resource::Budget{
                    descriptor.words[0], descriptor.words[1]},
                descriptor.words[2]}});
    }

    case ObjNotification: {
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

    case ObjIrq: {
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

    case ObjChannel: {
        if (!words_zero(descriptor, 3)
            || (descriptor.words[0] != CAP_CHANNEL_SIDE_A
                && descriptor.words[0] != CAP_CHANNEL_SIDE_B)) {
            return std::unexpected(AttenuationError::InvalidData);
        }
        const bool unbound = descriptor.words[1] == 0
            && descriptor.words[2] == 0;
        const bool exact = descriptor.words[1] != 0
            && descriptor.words[2] == ~u64{};
        if (!unbound && !exact) {
            return std::unexpected(AttenuationError::InvalidData);
        }
        const auto side = descriptor.words[0] == CAP_CHANNEL_SIDE_A
            ? ChannelSide::A : ChannelSide::B;
        return (View{
            child_rights,
            ChanLimit{side, descriptor.words[1], descriptor.words[2]}});
    }


    case ObjInvalid:
    case ObjCount:
        return std::unexpected(AttenuationError::UnsupportedKind);
    }
    return std::unexpected(AttenuationError::UnsupportedKind);
}

} // namespace cap
