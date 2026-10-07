#pragma once

#include <libk/bits.hpp>
#include <uapi/cap.h>

#include <variant>
#include <mm/types.hpp>
#include <resource/budget.hpp>
#include <expected>
#include <object/id.hpp>
#include <base/types.hpp>
#include <limits>
#include <optional>
#include <libk/span.hpp>

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
    Duplicate = u64{1} << 0,
    Delegate = u64{1} << 1,
    Reserve = u64{1} << 2,
    Map = u64{1} << 4,
    Unmap = u64{1} << 5,
    Protect = u64{1} << 6,
    Destroy = u64{1} << 7,
    Inspect = u64{1} << 8,
    Control = u64{1} << 9,
    Manage = u64{1} << 10,
    Revoke = u64{1} << 11,
    Create = u64{1} << 12,
    Split = u64{1} << 13,
    Close = u64{1} << 14,
    Signal = u64{1} << 15,
    Receive = u64{1} << 16,
    Connect = u64{1} << 17,
    Ack = u64{1} << 18,
    Call = u64{1} << 19,
    Send = u64{1} << 20,
    Serve = u64{1} << 21,
    Supply = u64{1} << 22,
    Fail = u64{1} << 23,
    WritebackAck = u64{1} << 24,
    Route = u64{1} << 25,
    Observe = u64{1} << 26,
    Attach = u64{1} << 27,
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

struct Badge final {
    u64 badge{};

    [[nodiscard]] friend constexpr auto operator==(
        Badge, Badge) noexcept
        -> bool = default;
};

// Endpoint capabilities carry the identity and admission limits presented to
// the service. `fixed` describes the badge bits already chosen by an ancestor;
// a fully fixed badge is callable, while a partial view is minting authority.
// Descendants may only fix more bits and reduce transfer capacity.
struct EpLimit final {
    u64 badge{};
    u64 fixed{};
    usize cap_limit{};

    [[nodiscard]] constexpr auto callable() const noexcept -> bool {
        return fixed == ~u64{};
    }

    [[nodiscard]] friend constexpr auto operator==(
        EpLimit, EpLimit) noexcept -> bool = default;
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

using Limits = std::variant<
    std::monostate,
    MemLimit,
    VmLimit,
    Quota,
    Badge,
    EpLimit,
    ChanLimit,
    IrqRoute>;

struct View final {
    Rights rights{};
    Limits data{};
};

enum class PolicyError : u8 {
    UnsupportedKind,
    InvalidRights,
    InvalidData,
    Amplification,
    Denied,
};

[[nodiscard]] auto validate_ceiling(
    object::ObjectKind kind,
    View ceiling) noexcept -> bool;

[[nodiscard]] auto compose(
    object::ObjectKind kind,
    View ceiling,
    View view) noexcept -> std::expected<View, PolicyError>;

// Checks a new Grant ceiling against the authority that the source slot can
// actually exercise. This is intentionally stronger than comparing it with
// the source Grant's original ceiling.
[[nodiscard]] auto attenuates(
    object::ObjectKind kind,
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
    libk::Span<const byte> bytes) noexcept
    -> std::expected<Attenuation, AttenuationError>;

[[nodiscard]] auto attenuation_kind(u16 raw) noexcept
    -> std::optional<object::ObjectKind>;

// Convert source-relative words into the existing authority representation.
// The policy layer remains the final source-relative containment and rights
// check; this function only performs closed-schema decoding and immutable
// identity inheritance.
[[nodiscard]] auto make_attenuation_ceiling(
    object::ObjectKind kind,
    const View& source,
    const Attenuation& descriptor) noexcept
    -> std::expected<View, AttenuationError>;

} // namespace cap
