#pragma once

#include <cap/authority.hpp>
#include <core/types.hpp>
#include <libk/expected.hpp>
#include <libk/limits.hpp>
#include <libk/optional.hpp>
#include <libk/span.hpp>
#include <object/object_id.hpp>

namespace kernel::cap {

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
    -> libk::Expected<Attenuation, AttenuationError>;

[[nodiscard]] auto attenuation_kind(u16 raw) noexcept
    -> libk::optional<object::ObjectKind>;

// Convert source-relative words into the existing authority representation.
// The policy layer remains the final source-relative containment and rights
// check; this function only performs closed-schema decoding and immutable
// identity inheritance.
[[nodiscard]] auto make_attenuation_ceiling(
    object::ObjectKind kind,
    const EffectiveAuthority& source,
    const Attenuation& descriptor) noexcept
    -> libk::Expected<GrantCeiling, AttenuationError>;

} // namespace kernel::cap
