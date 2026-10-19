#pragma once

#include <libk/bits.hpp>
#include <uapi/cap.h>

#include <variant>
#include <mm/types.hpp>
#include <resource/budget.hpp>
#include <expected>
#include <object/ref.hpp>
#include <libk/types.hpp>
#include <limits>
#include <optional>
#include <span>

namespace cap {

class Handle final {
public:
    static constexpr usize index_bits = 24;
    static constexpr usize generation_bits = 64 - index_bits;
    static constexpr u64 max_index = (u64{1} << index_bits) - 1;
    static constexpr u64 max_generation =
        (u64{1} << generation_bits) - 1;

    constexpr Handle() noexcept = default;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return value_ != 0;
    }
    [[nodiscard]] constexpr auto raw() const noexcept -> u64 {
        return value_;
    }
    [[nodiscard]] constexpr auto index() const noexcept -> usize {
        return static_cast<usize>(value_ & max_index);
    }
    [[nodiscard]] constexpr auto generation() const noexcept -> u64 {
        return value_ >> index_bits;
    }

    [[nodiscard]] static constexpr auto from_raw(u64 value) noexcept
        -> Handle {
        const Handle handle{value};
        return handle.generation() != 0 && handle.index() <= max_index
            ? handle
            : Handle{};
    }

    [[nodiscard]] friend constexpr auto operator==(
        Handle, Handle) noexcept -> bool = default;

private:
    friend class CSpace;

    [[nodiscard]] static constexpr auto make(
        usize index,
        u64 generation) noexcept -> Handle {
        return index <= max_index
                && generation != 0
                && generation <= max_generation
            ? Handle{(generation << index_bits) | index}
            : Handle{};
    }

    explicit constexpr Handle(u64 value) noexcept : value_(value) {}
    u64 value_{};
};

static_assert(sizeof(Handle) == sizeof(u64));
static_assert(!Handle{});

enum class Right : u64 {
    Duplicate = RIGHT_DUPLICATE,
    Delegate = RIGHT_DELEGATE,
    Reserve = RIGHT_RESERVE,
    Map = RIGHT_MAP,
    Unmap = RIGHT_UNMAP,
    Protect = RIGHT_PROTECT,
    Destroy = RIGHT_DESTROY,
    Inspect = RIGHT_INSPECT,
    Control = RIGHT_CONTROL,
    Manage = RIGHT_MANAGE,
    Revoke = RIGHT_REVOKE,
    Create = RIGHT_CREATE,
    Split = RIGHT_SPLIT,
    Close = RIGHT_CLOSE,
    Signal = RIGHT_SIGNAL,
    Receive = RIGHT_RECEIVE,
    Connect = RIGHT_CONNECT,
    Ack = RIGHT_ACK,
    Send = RIGHT_SEND,
    Serve = RIGHT_SERVE,
    Supply = RIGHT_SUPPLY,
    Fail = RIGHT_FAIL,
    WritebackAck = RIGHT_WRITEBACK_ACK,
    Route = RIGHT_ROUTE,
    Observe = RIGHT_OBSERVE,
    Attach = RIGHT_ATTACH,
    Adopt = RIGHT_ADOPT,
    Rebill = RIGHT_REBILL,
};

using Rights = libk::enum_flags<Right>;

template<class Range>
struct Limit final {
    Range range{};
    mm::Perms perms{};
    friend constexpr auto operator==(Limit, Limit) noexcept -> bool = default;
};
using MemLimit = Limit<mm::ObjectRange>;
using VmLimit = Limit<mm::VRange>;

struct Quota final {
    resource::budget budget{};
    u64 object_kinds{};

    [[nodiscard]] friend constexpr auto operator==(
        Quota, Quota) noexcept
        -> bool = default;
};

struct ScopeLimit final {
    u64 kinds{OBJ_KINDS};
    u32 facets{SCOPE_FACETS}, depth{64};
    friend constexpr auto operator==(ScopeLimit, ScopeLimit) noexcept -> bool = default;
};

struct Badge final {
    u64 badge{};

    [[nodiscard]] friend constexpr auto operator==(
        Badge, Badge) noexcept
        -> bool = default;
};

enum class ChannelSide : u8 {
    A,
    B,
    Any,
};

struct ChanLimit final {
    ChannelSide side{ChannelSide::A};
    u64 badge{};
    // zero means an unbound side-root; all bits set means an exact badge.
    u64 fixed{};

    [[nodiscard]] constexpr auto unbound() const noexcept -> bool {
        return fixed == 0 && badge == 0;
    }
    [[nodiscard]] constexpr auto exact() const noexcept -> bool {
        return fixed == ~u64{} && badge != 0;
    }

    [[nodiscard]] friend constexpr auto operator==(
        ChanLimit, ChanLimit) noexcept -> bool = default;
};

struct IrqRoute final {
    u32 source{};
    bool level{};

    [[nodiscard]] friend constexpr auto operator==(
        IrqRoute, IrqRoute) noexcept -> bool = default;
};

struct PhysLimit {
    mm::Pages range;
    friend constexpr auto operator==(PhysLimit, PhysLimit) noexcept -> bool = default;
};

struct IndexLimit {
    usize first{}, count{};
    friend constexpr auto operator==(IndexLimit, IndexLimit) noexcept -> bool = default;
};

using Limits = std::variant<
    std::monostate,
    MemLimit,
    PhysLimit,
    VmLimit,
    Quota,
    Badge,
    ChanLimit,
    IrqRoute, IndexLimit, ScopeLimit>;

struct View final {
    Rights rights{};
    Limits data{};
};

auto describe(ObjKind, View) noexcept -> CapView;

enum class PolicyError : u8 {
    UnsupportedKind,
    InvalidRights,
    InvalidData,
    Amplification,
    Denied,
};

[[nodiscard]] auto validate_ceiling(
    ObjKind kind,
    View ceiling) noexcept -> bool;

[[nodiscard]] auto compose(
    ObjKind kind,
    View ceiling,
    View view) noexcept -> std::expected<View, PolicyError>;

// Checks a new Grant ceiling against the authority that the source slot can
// actually exercise. This is intentionally stronger than comparing it with
// the source Grant's original ceiling.
[[nodiscard]] auto attenuates(
    ObjKind kind,
    View source,
    View child) noexcept -> bool;

enum class AttenuationError : u8 {
    InvalidSize,
    InvalidVersion,
    InvalidKind,
    InvalidRights,
    InvalidWord,
    InvalidRange,
    InvalidData,
    UnsupportedKind,
};

struct Attenuation final {
    u16 version{};
    u16 kind{};
    u32 size{};
    u64 rights{};
    u64 words[6]{};
};

[[nodiscard]] auto decode_attenuation(
    std::span<const byte> bytes) noexcept
    -> std::expected<Attenuation, AttenuationError>;

[[nodiscard]] auto attenuation_kind(u16 raw) noexcept
    -> std::optional<ObjKind>;

// Convert source-relative words into the existing authority representation.
// The policy layer remains the final source-relative containment and rights
// check; this function only performs closed-schema decoding and immutable
// identity inheritance.
[[nodiscard]] auto make_attenuation_ceiling(
    ObjKind kind,
    const View& source,
    const Attenuation& descriptor) noexcept
    -> std::expected<View, AttenuationError>;

} // namespace cap
