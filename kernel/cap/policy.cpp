#include <cap/policy.hpp>

namespace kernel::cap {

namespace {

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
    Right::CreateRegion,
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
    Right::Wait,
    Right::Destroy,
    Right::Revoke);

[[nodiscard]] auto valid(MemoryAuthority authority) noexcept -> bool {
    return authority.range.end().has_value()
        && kernel::mm::valid_access(authority.access)
        && kernel::mm::valid_memory_types(authority.types);
}

[[nodiscard]] auto valid(VSpaceAuthority authority) noexcept -> bool {
    return authority.region.valid()
        && authority.range.valid()
        && !authority.range.empty()
        && (authority.range.base().raw() & (kernel::mm::page_size - 1)) == 0
        && (authority.range.size() & (kernel::mm::page_size - 1)) == 0
        && kernel::mm::valid_access(authority.access)
        && kernel::mm::valid_memory_types(authority.types);
}

[[nodiscard]] auto valid(ResourcePoolAuthority authority) noexcept -> bool {
    constexpr u64 valid_kinds =
        ((u64{1} << static_cast<u16>(object::ObjectKind::Count)) - 1)
        & ~u64{1};
    return (authority.object_kinds & ~valid_kinds) == 0;
}

template<object::ObjectKind Kind>
[[nodiscard]] auto compose_as(
    GrantCeiling ceiling,
    CapView view) noexcept
    -> libk::Expected<EffectiveAuthority, PolicyError> {
    return CapabilityPolicy<Kind>::compose(ceiling, view);
}

} // namespace

auto CapabilityPolicy<object::ObjectKind::MemoryObject>::validate(
    GrantCeiling ceiling) noexcept -> bool {
    const auto* const data = libk::get_if<MemoryAuthority>(&ceiling.data);
    return memory_rights.contains(ceiling.rights)
        && data != nullptr && valid(*data);
}

auto CapabilityPolicy<object::ObjectKind::MemoryObject>::compose(
    GrantCeiling ceiling,
    CapView view) noexcept
    -> libk::Expected<EffectiveAuthority, PolicyError> {
    if (!memory_rights.contains(ceiling.rights)
        || !memory_rights.contains(view.rights)) {
        return libk::unexpected(PolicyError::InvalidRights);
    }
    const auto* const upper = libk::get_if<MemoryAuthority>(&ceiling.data);
    const auto* const local = libk::get_if<MemoryAuthority>(&view.data);
    if (upper == nullptr || local == nullptr
        || !valid(*upper) || !valid(*local)) {
        return libk::unexpected(PolicyError::InvalidData);
    }
    if (!ceiling.rights.contains(view.rights)
        || !upper->range.contains(local->range)
        || !upper->access.contains(local->access)
        || !upper->types.contains(local->types)) {
        return libk::unexpected(PolicyError::Amplification);
    }
    return libk::expected(EffectiveAuthority{
        ceiling.rights.intersect(view.rights), *local});
}

auto CapabilityPolicy<object::ObjectKind::VSpace>::validate(
    GrantCeiling ceiling) noexcept -> bool {
    const auto* const data = libk::get_if<VSpaceAuthority>(&ceiling.data);
    return vspace_rights.contains(ceiling.rights)
        && data != nullptr && valid(*data);
}

auto CapabilityPolicy<object::ObjectKind::VSpace>::compose(
    GrantCeiling ceiling,
    CapView view) noexcept
    -> libk::Expected<EffectiveAuthority, PolicyError> {
    if (!vspace_rights.contains(ceiling.rights)
        || !vspace_rights.contains(view.rights)) {
        return libk::unexpected(PolicyError::InvalidRights);
    }
    const auto* const upper = libk::get_if<VSpaceAuthority>(&ceiling.data);
    const auto* const local = libk::get_if<VSpaceAuthority>(&view.data);
    if (upper == nullptr || local == nullptr
        || !valid(*upper) || !valid(*local)) {
        return libk::unexpected(PolicyError::InvalidData);
    }
    if (!ceiling.rights.contains(view.rights)
        || upper->region != local->region
        || !upper->range.contains(local->range)
        || !upper->access.contains(local->access)
        || !upper->types.contains(local->types)) {
        return libk::unexpected(PolicyError::Amplification);
    }
    return libk::expected(EffectiveAuthority{
        ceiling.rights.intersect(view.rights), *local});
}

auto CapabilityPolicy<object::ObjectKind::ResourcePool>::validate(
    GrantCeiling ceiling) noexcept -> bool {
    const auto* const data = libk::get_if<ResourcePoolAuthority>(&ceiling.data);
    return resource_rights.contains(ceiling.rights)
        && data != nullptr && valid(*data);
}

auto CapabilityPolicy<object::ObjectKind::ResourcePool>::compose(
    GrantCeiling ceiling,
    CapView view) noexcept
    -> libk::Expected<EffectiveAuthority, PolicyError> {
    if (!resource_rights.contains(ceiling.rights)
        || !resource_rights.contains(view.rights)) {
        return libk::unexpected(PolicyError::InvalidRights);
    }
    const auto* const upper = libk::get_if<ResourcePoolAuthority>(
        &ceiling.data);
    const auto* const local = libk::get_if<ResourcePoolAuthority>(&view.data);
    if (upper == nullptr || local == nullptr
        || !valid(*upper) || !valid(*local)) {
        return libk::unexpected(PolicyError::InvalidData);
    }
    if (!ceiling.rights.contains(view.rights)
        || upper->budget.memory < local->budget.memory
        || upper->budget.caps < local->budget.caps
        || (local->object_kinds & ~upper->object_kinds) != 0) {
        return libk::unexpected(PolicyError::Amplification);
    }
    return libk::expected(EffectiveAuthority{
        ceiling.rights.intersect(view.rights), *local});
}

auto CapabilityPolicy<object::ObjectKind::Notification>::validate(
    GrantCeiling ceiling) noexcept -> bool {
    const auto* const data = libk::get_if<NotificationAuthority>(
        &ceiling.data);
    return notification_rights.contains(ceiling.rights)
        && data != nullptr && data->badge != 0;
}

auto CapabilityPolicy<object::ObjectKind::Notification>::compose(
    GrantCeiling ceiling,
    CapView view) noexcept
    -> libk::Expected<EffectiveAuthority, PolicyError> {
    if (!notification_rights.contains(ceiling.rights)
        || !notification_rights.contains(view.rights)) {
        return libk::unexpected(PolicyError::InvalidRights);
    }
    const auto* const upper = libk::get_if<NotificationAuthority>(
        &ceiling.data);
    const auto* const local = libk::get_if<NotificationAuthority>(&view.data);
    if (upper == nullptr || local == nullptr
        || upper->badge == 0 || local->badge == 0) {
        return libk::unexpected(PolicyError::InvalidData);
    }
    if (!ceiling.rights.contains(view.rights)
        || upper->badge != local->badge) {
        return libk::unexpected(PolicyError::Amplification);
    }
    return libk::expected(EffectiveAuthority{
        ceiling.rights.intersect(view.rights), *local});
}

auto validate_ceiling(
    object::ObjectKind kind,
    GrantCeiling ceiling) noexcept -> bool {
    switch (kind) {
    case object::ObjectKind::Thread:
        return CapabilityPolicy<object::ObjectKind::Thread>::validate(ceiling);
    case object::ObjectKind::Vproc:
        return CapabilityPolicy<object::ObjectKind::Vproc>::validate(ceiling);
    case object::ObjectKind::SchedulingContext:
        return CapabilityPolicy<
            object::ObjectKind::SchedulingContext>::validate(ceiling);
    case object::ObjectKind::SchedulingDomain:
        return CapabilityPolicy<
            object::ObjectKind::SchedulingDomain>::validate(ceiling);
    case object::ObjectKind::CSpace:
        return CapabilityPolicy<object::ObjectKind::CSpace>::validate(ceiling);
    case object::ObjectKind::MemoryObject:
        return CapabilityPolicy<
            object::ObjectKind::MemoryObject>::validate(ceiling);
    case object::ObjectKind::VSpace:
        return CapabilityPolicy<object::ObjectKind::VSpace>::validate(ceiling);
    case object::ObjectKind::ResourcePool:
        return CapabilityPolicy<
            object::ObjectKind::ResourcePool>::validate(ceiling);
    case object::ObjectKind::Notification:
        return CapabilityPolicy<
            object::ObjectKind::Notification>::validate(ceiling);
    case object::ObjectKind::Tunnel:
        return CapabilityPolicy<object::ObjectKind::Tunnel>::validate(ceiling);
    case object::ObjectKind::Invalid:
    case object::ObjectKind::Count:
        return false;
    }
    return false;
}

auto compose(
    object::ObjectKind kind,
    GrantCeiling ceiling,
    CapView view) noexcept
    -> libk::Expected<EffectiveAuthority, PolicyError> {
    switch (kind) {
    case object::ObjectKind::Thread:
        return compose_as<object::ObjectKind::Thread>(ceiling, view);
    case object::ObjectKind::Vproc:
        return compose_as<object::ObjectKind::Vproc>(ceiling, view);
    case object::ObjectKind::SchedulingContext:
        return compose_as<object::ObjectKind::SchedulingContext>(ceiling, view);
    case object::ObjectKind::SchedulingDomain:
        return compose_as<object::ObjectKind::SchedulingDomain>(ceiling, view);
    case object::ObjectKind::CSpace:
        return compose_as<object::ObjectKind::CSpace>(ceiling, view);
    case object::ObjectKind::MemoryObject:
        return compose_as<object::ObjectKind::MemoryObject>(ceiling, view);
    case object::ObjectKind::VSpace:
        return compose_as<object::ObjectKind::VSpace>(ceiling, view);
    case object::ObjectKind::ResourcePool:
        return compose_as<object::ObjectKind::ResourcePool>(ceiling, view);
    case object::ObjectKind::Notification:
        return compose_as<object::ObjectKind::Notification>(ceiling, view);
    case object::ObjectKind::Tunnel:
        return compose_as<object::ObjectKind::Tunnel>(ceiling, view);
    case object::ObjectKind::Invalid:
    case object::ObjectKind::Count:
        return libk::unexpected(PolicyError::UnsupportedKind);
    }
    return libk::unexpected(PolicyError::UnsupportedKind);
}

auto attenuates(
    object::ObjectKind kind,
    EffectiveAuthority source,
    GrantCeiling child) noexcept -> bool {
    auto composed = compose(
        kind,
        source.ceiling(),
        CapView{child.rights, child.data});
    return composed
        && composed.value().rights == child.rights
        && composed.value().data == child.data;
}

} // namespace kernel::cap
