#pragma once

#include <cap/authority.hpp>
#include <libk/expected.hpp>
#include <object/object_id.hpp>

namespace kernel::cap {

enum class PolicyError : u8 {
    UnsupportedKind,
    InvalidRights,
    InvalidData,
    Amplification,
    Denied,
};

template<object::ObjectKind Kind>
struct CapabilityPolicy;

template<Right... Allowed>
struct RightsPolicy {
    [[nodiscard]] static constexpr auto allowed() noexcept -> Rights {
        return Rights::of(Allowed...);
    }

    [[nodiscard]] static constexpr auto validate(Rights rights) noexcept
        -> bool {
        return allowed().contains(rights);
    }

    [[nodiscard]] static auto validate(GrantCeiling ceiling) noexcept -> bool {
        return validate(ceiling.rights)
            && libk::holds_alternative<libk::monostate>(ceiling.data);
    }

    [[nodiscard]] static auto compose(
        GrantCeiling ceiling,
        CapView view) noexcept
        -> libk::Expected<EffectiveAuthority, PolicyError> {
        if (!validate(ceiling.rights) || !validate(view.rights)) {
            return libk::unexpected(PolicyError::InvalidRights);
        }
        if (!libk::holds_alternative<libk::monostate>(ceiling.data)
            || !libk::holds_alternative<libk::monostate>(view.data)) {
            return libk::unexpected(PolicyError::InvalidData);
        }
        if (!ceiling.rights.contains(view.rights)) {
            return libk::unexpected(PolicyError::Amplification);
        }
        return libk::expected(EffectiveAuthority{
            ceiling.rights.intersect(view.rights), libk::monostate{}});
    }
};

template<>
struct CapabilityPolicy<object::ObjectKind::Thread> final
    : RightsPolicy<
          Right::Duplicate, Right::Delegate, Right::Inspect, Right::Control,
          Right::Destroy, Right::Revoke> {};

template<>
struct CapabilityPolicy<object::ObjectKind::Vproc> final
    : RightsPolicy<
          Right::Duplicate, Right::Delegate, Right::Inspect, Right::Control,
          Right::Destroy, Right::Revoke> {};

template<>
struct CapabilityPolicy<object::ObjectKind::Tunnel> final
    : RightsPolicy<
          Right::Duplicate, Right::Delegate, Right::Inspect, Right::Signal,
          Right::Connect, Right::Ack, Right::Close, Right::Destroy,
          Right::Revoke> {};

template<>
struct CapabilityPolicy<object::ObjectKind::Endpoint> final {
    [[nodiscard]] static auto validate(GrantCeiling ceiling) noexcept -> bool;
    [[nodiscard]] static auto compose(
        GrantCeiling ceiling,
        CapView view) noexcept
        -> libk::Expected<EffectiveAuthority, PolicyError>;
};

template<>
struct CapabilityPolicy<object::ObjectKind::SchedulingContext> final
    : RightsPolicy<
          Right::Duplicate, Right::Delegate, Right::Inspect, Right::Control,
          Right::Destroy, Right::Revoke> {};

template<>
struct CapabilityPolicy<object::ObjectKind::SchedulingDomain> final
    : RightsPolicy<
          Right::Duplicate, Right::Delegate, Right::Inspect, Right::Control,
          Right::Destroy, Right::Revoke> {};

template<>
struct CapabilityPolicy<object::ObjectKind::CSpace> final
    : RightsPolicy<
          Right::Duplicate, Right::Delegate, Right::Inspect, Right::Manage,
          Right::Destroy, Right::Revoke> {};

template<>
struct CapabilityPolicy<object::ObjectKind::MemoryObject> final {
    [[nodiscard]] static auto validate(GrantCeiling ceiling) noexcept -> bool;
    [[nodiscard]] static auto compose(
        GrantCeiling ceiling,
        CapView view) noexcept
        -> libk::Expected<EffectiveAuthority, PolicyError>;
};

template<>
struct CapabilityPolicy<object::ObjectKind::VSpace> final {
    [[nodiscard]] static auto validate(GrantCeiling ceiling) noexcept -> bool;
    [[nodiscard]] static auto compose(
        GrantCeiling ceiling,
        CapView view) noexcept
        -> libk::Expected<EffectiveAuthority, PolicyError>;
};

template<>
struct CapabilityPolicy<object::ObjectKind::ResourcePool> final {
    [[nodiscard]] static auto validate(GrantCeiling ceiling) noexcept -> bool;
    [[nodiscard]] static auto compose(
        GrantCeiling ceiling,
        CapView view) noexcept
        -> libk::Expected<EffectiveAuthority, PolicyError>;
};

template<>
struct CapabilityPolicy<object::ObjectKind::Notification> final {
    [[nodiscard]] static auto validate(GrantCeiling ceiling) noexcept -> bool;
    [[nodiscard]] static auto compose(
        GrantCeiling ceiling,
        CapView view) noexcept
        -> libk::Expected<EffectiveAuthority, PolicyError>;
};

[[nodiscard]] auto validate_ceiling(
    object::ObjectKind kind,
    GrantCeiling ceiling) noexcept -> bool;

[[nodiscard]] auto compose(
    object::ObjectKind kind,
    GrantCeiling ceiling,
    CapView view) noexcept -> libk::Expected<EffectiveAuthority, PolicyError>;

// Checks a new Grant ceiling against the authority that the source slot can
// actually exercise. This is intentionally stronger than comparing it with
// the source Grant's original ceiling.
[[nodiscard]] auto attenuates(
    object::ObjectKind kind,
    EffectiveAuthority source,
    GrantCeiling child) noexcept -> bool;

} // namespace kernel::cap
