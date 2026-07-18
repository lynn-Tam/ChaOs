#include "kernel/syscall/internal.hpp"

#include <cap/policy.hpp>
#include <core/kernel_state.hpp>
#include <cpu/cpu_local.hpp>
#include <cpu/cpu_runtime.hpp>
#include <execution/vproc.hpp>
#include <libk/checked_arithmetic.hpp>
#include <libk/utility.hpp>
#include <ipc/notification.hpp>
#include <ipc/tunnel.hpp>
#include <mm/virtual_layout.hpp>
#include <mm/kernel_stack.hpp>
#include <mm/memory_object.hpp>
#include <mm/vspace.hpp>
#include <object/object_store.hpp>
#include <resource/traits.hpp>
#include <sched/context.hpp>
#include <sched/domain.hpp>
#include <thread/thread.hpp>
#include <uapi/syscall.h>
#include <uapi/thread.h>
#include <uapi/tunnel.h>
#include <uapi/vproc.h>
#include <uapi/resource.h>

namespace kernel::syscall {
namespace {

static_assert(MYOS_RESOURCE_THREAD
    == (u64{1} << static_cast<u16>(object::ObjectKind::Thread)));
static_assert(MYOS_RESOURCE_SCHED_CONTEXT
    == (u64{1}
        << static_cast<u16>(object::ObjectKind::SchedulingContext)));
static_assert(MYOS_RESOURCE_CSPACE
    == (u64{1} << static_cast<u16>(object::ObjectKind::CSpace)));
static_assert(MYOS_RESOURCE_MEMORY
    == (u64{1} << static_cast<u16>(object::ObjectKind::MemoryObject)));
static_assert(MYOS_RESOURCE_VSPACE
    == (u64{1} << static_cast<u16>(object::ObjectKind::VSpace)));
static_assert(MYOS_RESOURCE_POOL
    == (u64{1} << static_cast<u16>(object::ObjectKind::ResourcePool)));
static_assert(MYOS_RESOURCE_NOTIFICATION
    == (u64{1} << static_cast<u16>(object::ObjectKind::Notification)));
static_assert(MYOS_RESOURCE_VPROC
    == (u64{1} << static_cast<u16>(object::ObjectKind::Vproc)));
static_assert(MYOS_RESOURCE_TUNNEL
    == (u64{1} << static_cast<u16>(object::ObjectKind::Tunnel)));

using kernel::object::ObjectKind;

[[nodiscard]] constexpr auto kind_bit(ObjectKind kind) noexcept -> u64 {
    return u64{1} << static_cast<u16>(kind);
}

[[nodiscard]] auto pool_authority(
    const cap::Resolved<kernel::resource::ResourcePool>& pool) noexcept
    -> libk::optional<cap::ResourcePoolAuthority> {
    const cap::EffectiveAuthority effective = pool.authority();
    const auto* const authority =
        libk::get_if<cap::ResourcePoolAuthority>(&effective.data);
    return authority != nullptr
        ? libk::optional<cap::ResourcePoolAuthority>{*authority}
        : libk::nullopt;
}

[[nodiscard]] constexpr auto within(
    kernel::resource::Budget charge,
    kernel::resource::Budget ceiling) noexcept -> bool {
    return charge.memory <= ceiling.memory && charge.caps <= ceiling.caps;
}

[[nodiscard]] auto add_budget(
    kernel::resource::Budget first,
    kernel::resource::Budget second) noexcept
    -> libk::optional<kernel::resource::Budget> {
    const auto memory = libk::checked_add(first.memory, second.memory);
    const auto caps = libk::checked_add(first.caps, second.caps);
    return memory && caps
        ? libk::optional<kernel::resource::Budget>{
              kernel::resource::Budget{*memory, *caps}}
        : libk::nullopt;
}

[[nodiscard]] auto reserve(
    cap::Resolved<kernel::resource::ResourcePool>& pool,
    kernel::resource::Budget charge) noexcept
    -> libk::Expected<kernel::resource::Reservation,
        kernel::resource::PoolError> {
    auto reference = pool.reference();
    if (!reference) {
        return libk::unexpected(kernel::resource::PoolError::InvalidPool);
    }
    return pool->reserve(libk::move(reference).value(), charge);
}

[[nodiscard]] auto begin(
    cap::Resolved<kernel::resource::ResourcePool>& pool) noexcept
    -> libk::Expected<kernel::resource::Permit,
        kernel::resource::PoolError> {
    auto reference = pool.reference();
    if (!reference) {
        return libk::unexpected(kernel::resource::PoolError::InvalidPool);
    }
    return pool->begin(libk::move(reference).value());
}

[[nodiscard]] auto pool_error(kernel::resource::PoolError error) noexcept
    -> myos_status_t {
    switch (error) {
    case kernel::resource::PoolError::InvalidPool:
        return MYOS_STATUS_INVALID_CAP;
    case kernel::resource::PoolError::Closed:
    case kernel::resource::PoolError::OutstandingAllocations:
        return MYOS_STATUS_BUSY;
    case kernel::resource::PoolError::Exhausted:
        return MYOS_STATUS_NO_MEMORY;
    case kernel::resource::PoolError::Overflow:
        return MYOS_STATUS_BAD_ARGS;
    }
    return MYOS_STATUS_INTERNAL;
}

struct PublishedAuthority final {
    cap::GrantCeiling ceiling{};
    cap::CapView view{};
};

template<
    kernel::resource::SponsoredObject T,
    typename Factory,
    typename Prepare,
    typename Authority>
[[nodiscard]] auto construct_with(
    Invocation& invocation,
    cap::Resolved<kernel::resource::ResourcePool>& pool,
    kernel::resource::Budget charge,
    kernel::resource::Budget authority_charge,
    Factory&& factory,
    Prepare&& prepare,
    Authority&& authority) noexcept -> Result {
    KernelState* const kernel = invocation.cpu.runtime().kernel;
    KASSERT(kernel != nullptr);
    const auto allowed = pool_authority(pool);
    if (!allowed
        || (allowed->object_kinds & kind_bit(object::ObjectTraits<T>::kind)) == 0
        || !within(authority_charge, allowed->budget)) {
        return returned(MYOS_STATUS_DENIED);
    }

    auto permit = begin(pool);
    if (!permit) {
        return returned(pool_error(permit.error()));
    }

    auto cap_slot = invocation.cspace.reserve();
    if (!cap_slot) {
        return returned(cap_status(cap_slot.error()));
    }
    auto object_charge = reserve(pool, charge);
    if (!object_charge) {
        return returned(pool_error(object_charge.error()));
    }
    auto root_charge = reserve(pool, kernel->grants().node_charge());
    if (!root_charge) {
        return returned(pool_error(root_charge.error()));
    }
    auto user_grant_charge = reserve(pool, kernel->grants().node_charge());
    if (!user_grant_charge) {
        return returned(pool_error(user_grant_charge.error()));
    }

    auto pending = factory(libk::move(object_charge).value());
    if (!pending) {
        return returned(MYOS_STATUS_NO_MEMORY);
    }
    const myos_status_t prepared = prepare(pending.value().get());
    if (prepared != MYOS_STATUS_OK) {
        return returned(prepared);
    }
    auto object = libk::move(pending).value().publish();
    const PublishedAuthority published = authority(object.get());
    auto reference = object.ref();
    if (!reference) {
        KASSERT(object.retire());
        object.reset();
        return returned(MYOS_STATUS_INTERNAL);
    }
    auto allocation = kernel->grants().create_allocation(
        permit.value(),
        libk::move(root_charge).value(),
        libk::move(reference).value(),
        published.ceiling);
    if (!allocation) {
        KASSERT(object.retire());
        object.reset();
        return returned(MYOS_STATUS_NO_MEMORY);
    }
    auto root_lease = allocation.value().acquire();
    auto user_target = object.ref();
    if (!root_lease || !user_target) {
        allocation.value().reset();
        object.reset();
        return returned(MYOS_STATUS_INTERNAL);
    }
    auto user_grant = kernel->grants().derive(
        libk::move(user_grant_charge).value(),
        root_lease.value(),
        libk::move(user_target).value(),
        published.ceiling);
    root_lease.value().reset();
    if (!user_grant) {
        allocation.value().reset();
        object.reset();
        return returned(MYOS_STATUS_NO_MEMORY);
    }
    auto installed = invocation.cspace.insert(
        libk::move(cap_slot).value(),
        libk::move(user_grant).value(),
        published.view);
    if (!installed) {
        allocation.value().reset();
        object.reset();
        return returned(cap_status(installed.error()));
    }
    allocation.value().commit();
    object.reset();
    return returned(MYOS_STATUS_OK, installed.value().raw());
}

template<kernel::resource::SponsoredObject T, typename Factory, typename Authority>
[[nodiscard]] auto construct(
    Invocation& invocation,
    cap::Resolved<kernel::resource::ResourcePool>& pool,
    kernel::resource::Budget charge,
    kernel::resource::Budget authority_charge,
    Factory&& factory,
    Authority&& authority) noexcept -> Result {
    return construct_with<T>(
        invocation,
        pool,
        charge,
        authority_charge,
        libk::forward<Factory>(factory),
        [](T&) noexcept -> myos_status_t { return MYOS_STATUS_OK; },
        libk::forward<Authority>(authority));
}

[[nodiscard]] auto resolve_pool(
    Invocation& invocation,
    cap::Right right) noexcept
    -> libk::Expected<cap::Resolved<kernel::resource::ResourcePool>,
        cap::CSpaceError> {
    return invocation.cspace.resolve<kernel::resource::ResourcePool>(
        handle_of(invocation.trap.arg(0)), cap::Rights::of(right));
}

[[nodiscard]] constexpr auto basic_rights() noexcept -> cap::Rights {
    return cap::Rights::of(
        cap::Right::Duplicate,
        cap::Right::Delegate,
        cap::Right::Inspect,
        cap::Right::Control,
        cap::Right::Destroy,
        cap::Right::Revoke);
}

[[nodiscard]] constexpr auto vproc_rights() noexcept -> cap::Rights {
    return cap::Rights::of(
        cap::Right::Duplicate,
        cap::Right::Delegate,
        cap::Right::Inspect,
        cap::Right::Control,
        cap::Right::Bind,
        cap::Right::Destroy,
        cap::Right::Revoke);
}

[[gnu::noinline]] [[nodiscard]] auto create_child(
    Invocation& invocation) noexcept -> Result {
    KernelState* const kernel = invocation.cpu.runtime().kernel;
    KASSERT(kernel != nullptr);
    arch::TrapContext& trap = invocation.trap;
    auto pool = resolve_pool(invocation, cap::Right::Split);
    if (!pool) {
        return returned(cap_status(pool.error()));
    }
    const kernel::resource::Budget limit{
        .memory = trap.arg(1), .caps = trap.arg(2)};
    const u64 kinds = trap.arg(3);
    const auto parent = pool_authority(pool.value());
    if (!parent || !within(limit, parent->budget)
        || (kinds & ~parent->object_kinds) != 0 || kinds == 0) {
        return returned(MYOS_STATUS_DENIED);
    }
    const auto charge = add_budget(
        kernel::resource::Traits<kernel::resource::ResourcePool>::fixed(),
        limit);
    if (!charge) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    const auto rights = cap::Rights::of(
        cap::Right::Duplicate,
        cap::Right::Delegate,
        cap::Right::Inspect,
        cap::Right::Create,
        cap::Right::Split,
        cap::Right::Close,
        cap::Right::Revoke);
    return construct<kernel::resource::ResourcePool>(
        invocation,
        pool.value(),
        *charge,
        limit,
        [&](kernel::resource::Reservation&& sponsorship) {
            return kernel->objects().create_resource_sponsored(
                libk::move(sponsorship), limit);
        },
        [&](kernel::resource::ResourcePool&) {
            const cap::ResourcePoolAuthority data{limit, kinds};
            return PublishedAuthority{
                cap::GrantCeiling{rights, data},
                cap::CapView{rights, data}};
        });
}

[[gnu::noinline]] [[nodiscard]] auto create_memory(
    Invocation& invocation) noexcept -> Result {
    KernelState* const kernel = invocation.cpu.runtime().kernel;
    KASSERT(kernel != nullptr);
    arch::TrapContext& trap = invocation.trap;
    auto pool = resolve_pool(invocation, cap::Right::Create);
    if (!pool) {
        return returned(cap_status(pool.error()));
    }
    const usize size = trap.arg(1);
    const auto access = access_of(trap.arg(2));
    if (size == 0 || !access) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    return construct<kernel::mm::MemoryObject>(
        invocation,
        pool.value(),
        kernel::resource::Traits<kernel::mm::MemoryObject>::fixed(),
        kernel::resource::Traits<kernel::mm::MemoryObject>::fixed(),
        [&](kernel::resource::Reservation&& sponsorship) {
            return kernel->objects().create_anonymous_sponsored(
                libk::move(sponsorship),
                size,
                kernel::mm::AnonymousConfig{.access = *access});
        },
        [&](kernel::mm::MemoryObject& memory) {
            const kernel::mm::MemoryTypes types =
                kernel::mm::MemoryTypes::of(kernel::mm::MemoryType::Normal);
            const cap::MemoryAuthority data{
                kernel::mm::ObjectRange{0, memory.page_count()},
                *access,
                types};
            const auto rights = cap::Rights::of(
                cap::Right::Duplicate,
                cap::Right::Delegate,
                cap::Right::Inspect,
                cap::Right::Map,
                cap::Right::Destroy,
                cap::Right::Manage,
                cap::Right::Revoke);
            return PublishedAuthority{
                cap::GrantCeiling{rights, data},
                cap::CapView{rights, data}};
        });
}

[[gnu::noinline]] [[nodiscard]] auto create_vspace(
    Invocation& invocation) noexcept -> Result {
    KernelState* const kernel = invocation.cpu.runtime().kernel;
    KASSERT(kernel != nullptr);
    auto pool = resolve_pool(invocation, cap::Right::Create);
    if (!pool) {
        return returned(cap_status(pool.error()));
    }
    return construct<kernel::mm::VSpace>(
        invocation,
        pool.value(),
        kernel::resource::Traits<kernel::mm::VSpace>::fixed(),
        kernel::resource::Traits<kernel::mm::VSpace>::fixed(),
        [&](kernel::resource::Reservation&& sponsorship) {
            return kernel->objects().create_vspace_sponsored(
                libk::move(sponsorship), kernel->kernel_vspace());
        },
        [&](kernel::mm::VSpace& space) {
            const cap::VSpaceAuthority data{
                space.root_key(),
                kernel::mm::VirtRange{
                    kernel::mm::VirtAddr{kernel::mm::layout::LowGuardEnd},
                    kernel::mm::layout::UserEnd
                        - kernel::mm::layout::LowGuardEnd},
                kernel::mm::AccessMask::of(
                    kernel::mm::Access::Read,
                    kernel::mm::Access::Write,
                    kernel::mm::Access::Execute),
                kernel::mm::MemoryTypes::of(kernel::mm::MemoryType::Normal)};
            const auto rights = cap::Rights::of(
                cap::Right::Duplicate,
                cap::Right::Delegate,
                cap::Right::Reserve,
                cap::Right::CreateRegion,
                cap::Right::Map,
                cap::Right::Unmap,
                cap::Right::Protect,
                cap::Right::Inspect,
                cap::Right::Manage,
                cap::Right::Destroy,
                cap::Right::Revoke);
            return PublishedAuthority{
                cap::GrantCeiling{rights, data}, cap::CapView{rights, data}};
        });
}

[[gnu::noinline]] [[nodiscard]] auto create_cspace(
    Invocation& invocation) noexcept -> Result {
    KernelState* const kernel = invocation.cpu.runtime().kernel;
    KASSERT(kernel != nullptr);
    arch::TrapContext& trap = invocation.trap;
    auto pool = resolve_pool(invocation, cap::Right::Create);
    if (!pool) {
        return returned(cap_status(pool.error()));
    }
    const cap::CSpace::Quota quota{
        .slots = trap.arg(1), .pages = trap.arg(2)};
    if (quota.slots == 0 || quota.pages == 0) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    const auto rights = cap::Rights::of(
        cap::Right::Duplicate,
        cap::Right::Delegate,
        cap::Right::Inspect,
        cap::Right::Manage,
        cap::Right::Destroy,
        cap::Right::Revoke);
    return construct<cap::CSpace>(
        invocation,
        pool.value(),
        kernel::resource::Traits<cap::CSpace>::fixed(),
        kernel::resource::Traits<cap::CSpace>::fixed(),
        [&](kernel::resource::Reservation&& sponsorship) {
            return kernel->objects().create_cspace_sponsored(
                libk::move(sponsorship), quota);
        },
        [&](cap::CSpace&) {
            return PublishedAuthority{
                cap::GrantCeiling{rights}, cap::CapView{rights}};
        });
}

[[gnu::noinline]] [[nodiscard]] auto create_sc(
    Invocation& invocation) noexcept -> Result {
    KernelState* const kernel = invocation.cpu.runtime().kernel;
    KASSERT(kernel != nullptr);
    arch::TrapContext& trap = invocation.trap;
    auto pool = resolve_pool(invocation, cap::Right::Create);
    auto domain = invocation.cspace.resolve<kernel::sched::SchedulingDomain>(
        handle_of(trap.arg(1)), cap::Rights::of(cap::Right::Control));
    if (!pool || !domain) {
        return returned(cap_status(!pool ? pool.error() : domain.error()));
    }
    const auto budget = kernel->clock().duration_from_nanoseconds(trap.arg(2));
    const auto period = kernel->clock().duration_from_nanoseconds(trap.arg(3));
    const auto urgency = kernel::sched::Urgency::make(trap.arg(4));
    const kernel::CpuId home{trap.arg(5)};
    if (!budget || !period || !urgency) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    const kernel::sched::SchedulingContext::Config config{
        .budget = *budget, .period = *period, .urgency = *urgency};
    if (!kernel::sched::SchedulingContext::valid_config(config)) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    const auto rights = basic_rights();
    return construct_with<kernel::sched::SchedulingContext>(
        invocation,
        pool.value(),
        kernel::resource::Traits<kernel::sched::SchedulingContext>::fixed(),
        kernel::resource::Traits<kernel::sched::SchedulingContext>::fixed(),
        [&](kernel::resource::Reservation&& sponsorship) {
            return kernel->objects().create_context_sponsored(
                libk::move(sponsorship), config, kernel->clock().now());
        },
        [&](kernel::sched::SchedulingContext& context) noexcept
            -> myos_status_t {
            auto admitted = context.admit(domain.value(), home);
            if (admitted) {
                return MYOS_STATUS_OK;
            }
            return admitted.error()
                    == kernel::sched::SchedulingContext::Error::WrongCpu
                ? MYOS_STATUS_BAD_ARGS
                : MYOS_STATUS_BUSY;
        },
        [&](kernel::sched::SchedulingContext&) {
            return PublishedAuthority{
                cap::GrantCeiling{rights}, cap::CapView{rights}};
        });
}

template<typename Descriptor>
[[nodiscard]] auto read_snapshot(
    Invocation& invocation,
    cap::CapHandle handle,
    usize offset) noexcept -> libk::Expected<Descriptor, myos_status_t> {
    auto resolved = invocation.cspace.resolve<kernel::mm::MemoryObject>(
        handle, cap::Rights::of(cap::Right::Inspect));
    if (!resolved) {
        return libk::unexpected(cap_status(resolved.error()));
    }
    auto& source = resolved.value();
    const cap::EffectiveAuthority effective = source.authority();
    const auto* const authority = libk::get_if<cap::MemoryAuthority>(
        &effective.data);
    const auto end = libk::checked_add(offset, sizeof(Descriptor));
    if (authority == nullptr || !end
        || !authority->access.contains(kernel::mm::Access::Read)) {
        return libk::unexpected(MYOS_STATUS_DENIED);
    }
    const auto rounded = libk::checked_add(
        *end, kernel::mm::page_size - 1);
    if (!rounded) {
        return libk::unexpected(MYOS_STATUS_BAD_ARGS);
    }
    const usize first = offset / kernel::mm::page_size;
    const usize last = *rounded / kernel::mm::page_size;
    if (!authority->range.contains(kernel::mm::ObjectRange{
            first, last - first})) {
        return libk::unexpected(MYOS_STATUS_DENIED);
    }

    Descriptor descriptor{};
    auto read = source->read(
        offset,
        libk::Span<byte>{
            reinterpret_cast<byte*>(&descriptor), sizeof(descriptor)});
    if (!read) {
        switch (read.error()) {
        case kernel::mm::MemoryError::OutOfMemory:
        case kernel::mm::MemoryError::ResourceExhausted:
            return libk::unexpected(MYOS_STATUS_NO_MEMORY);
        case kernel::mm::MemoryError::BackingFailed:
            return libk::unexpected(MYOS_STATUS_BACKING_FAILED);
        case kernel::mm::MemoryError::Busy:
            return libk::unexpected(MYOS_STATUS_BUSY);
        default:
            return libk::unexpected(MYOS_STATUS_BAD_ARGS);
        }
    }
    return libk::expected(descriptor);
}

[[nodiscard]] auto start_snapshot(
    Invocation& invocation,
    cap::CapHandle handle,
    usize offset) noexcept -> libk::Expected<arch::UserStart, myos_status_t> {
    auto snapshot = read_snapshot<myos_thread_start>(
        invocation, handle, offset);
    if (!snapshot) {
        return libk::unexpected(snapshot.error());
    }
    const myos_thread_start& descriptor = snapshot.value();
    if (descriptor.version != MYOS_THREAD_START_VERSION
        || descriptor.flags != 0) {
        return libk::unexpected(MYOS_STATUS_BAD_ARGS);
    }
    arch::UserStart start{
        .entry = kernel::mm::VirtAddr{descriptor.entry},
        .stack = kernel::mm::VirtAddr{descriptor.stack},
    };
    for (usize index = 0; index < 6; ++index) {
        start.arguments[index] = descriptor.arguments[index];
    }
    if (!arch::valid_user_start(start)) {
        return libk::unexpected(MYOS_STATUS_BAD_ARGS);
    }
    return libk::expected(start);
}

[[nodiscard]] auto page_authorized(
    const cap::Resolved<kernel::mm::MemoryObject>& memory,
    usize page,
    kernel::mm::AccessMask access) noexcept -> bool {
    const cap::EffectiveAuthority effective = memory.authority();
    const auto* const authority =
        libk::get_if<cap::MemoryAuthority>(&effective.data);
    return authority != nullptr && authority->range.contains(
        kernel::mm::ObjectRange{page, 1})
        && authority->access.contains(access)
        && authority->types.contains(kernel::mm::MemoryType::Normal);
}

[[nodiscard]] auto memory_status(kernel::mm::MemoryError error) noexcept
    -> myos_status_t {
    switch (error) {
    case kernel::mm::MemoryError::OutOfMemory:
    case kernel::mm::MemoryError::ResourceExhausted:
        return MYOS_STATUS_NO_MEMORY;
    case kernel::mm::MemoryError::BackingFailed:
        return MYOS_STATUS_BACKING_FAILED;
    case kernel::mm::MemoryError::Busy:
        return MYOS_STATUS_BUSY;
    default:
        return MYOS_STATUS_BAD_ARGS;
    }
}

[[gnu::noinline]] [[nodiscard]] auto prepare_vproc_runtime(
    KernelState& kernel,
    cap::Resolved<kernel::mm::VSpace>& vspace,
    cap::Resolved<kernel::mm::MemoryObject>& control,
    cap::Resolved<kernel::mm::MemoryObject>& events,
    const myos_vproc_start& descriptor) noexcept
    -> libk::Expected<kernel::VprocRuntime, myos_status_t> {
    const auto writable = kernel::mm::AccessMask::of(
        kernel::mm::Access::Read, kernel::mm::Access::Write);
    if (!page_authorized(control, descriptor.control_page, writable)
        || !page_authorized(events, descriptor.event_page, writable)
        || (&control.object() == &events.object()
            && descriptor.control_page == descriptor.event_page)) {
        return libk::unexpected(MYOS_STATUS_BAD_RIGHTS);
    }

    auto control_ref = control.reference();
    auto event_ref = events.reference();
    if (!control_ref || !event_ref) {
        return libk::unexpected(MYOS_STATUS_BUSY);
    }
    const kernel::mm::VirtAddr control_address{descriptor.control_address};
    const kernel::mm::VirtAddr event_address{descriptor.event_address};
    auto control_view = vspace->bind_view(kernel::mm::UserViewRequest{
        .memory = libk::move(control_ref).value(),
        .object = kernel::mm::ObjectRange{descriptor.control_page, 1},
        .virtual_range = kernel::mm::VirtRange{
            control_address, kernel::mm::page_size},
        .access = writable,
    });
    if (!control_view) {
        return libk::unexpected(vm_status(control_view.error()));
    }
    const auto readable = kernel::mm::AccessMask::of(
        kernel::mm::Access::Read);
    auto event_view = vspace->bind_view(kernel::mm::UserViewRequest{
        .memory = libk::move(event_ref).value(),
        .object = kernel::mm::ObjectRange{descriptor.event_page, 1},
        .virtual_range = kernel::mm::VirtRange{
            event_address, kernel::mm::page_size},
        .access = readable,
    });
    if (!event_view) {
        return libk::unexpected(vm_status(event_view.error()));
    }

    auto control_page = control->materialize(descriptor.control_page);
    auto event_page = events->materialize(descriptor.event_page);
    if (!control_page || !event_page) {
        return libk::unexpected(memory_status(
            !control_page ? control_page.error() : event_page.error()));
    }
    auto* const control_bytes = kernel.pmm().bytes(
        control_page.value().page().page);
    auto* const event_bytes = kernel.pmm().bytes(
        event_page.value().page().page);
    KASSERT(control_bytes != nullptr && event_bytes != nullptr);
    kernel::VprocRuntime runtime{};
    runtime.control_view = libk::move(control_view).value();
    runtime.event_view = libk::move(event_view).value();
    runtime.control_page = libk::move(control_page).value();
    runtime.event_page = libk::move(event_page).value();
    runtime.control =
        reinterpret_cast<myos_vproc_control_page*>(control_bytes);
    runtime.events = reinterpret_cast<myos_vproc_event_page*>(event_bytes);
    runtime.control_address = control_address;
    runtime.event_address = event_address;
    return libk::expected(libk::move(runtime));
}

[[gnu::noinline]] [[nodiscard]] auto create_thread(
    Invocation& invocation) noexcept -> Result {
    KernelState* const kernel = invocation.cpu.runtime().kernel;
    KASSERT(kernel != nullptr);
    arch::TrapContext& trap = invocation.trap;
    auto pool = resolve_pool(invocation, cap::Right::Create);
    auto vspace = invocation.cspace.resolve<kernel::mm::VSpace>(
        handle_of(trap.arg(1)), cap::Rights::of(cap::Right::Manage));
    auto cspace = invocation.cspace.resolve<cap::CSpace>(
        handle_of(trap.arg(2)), cap::Rights::of(cap::Right::Manage));
    if (!pool || !vspace || !cspace) {
        const cap::CSpaceError error = !pool
            ? pool.error()
            : !vspace ? vspace.error()
            : cspace.error();
        return returned(cap_status(error));
    }
    auto start = start_snapshot(
        invocation, handle_of(trap.arg(3)), trap.arg(4));
    if (!start || trap.arg(5) != 0) {
        return returned(start ? MYOS_STATUS_BAD_ARGS : start.error());
    }

    auto stack_reservation = reserve(pool.value(), kernel::resource::Budget{
        .memory = kernel::mm::KernelStackLayout::StackBytes});
    if (!stack_reservation) {
        return returned(pool_error(stack_reservation.error()));
    }
    auto stack_capacity = libk::move(stack_reservation).value();
    auto home = kernel::KernelStack::create(kernel->kernel_vspace());
    auto address_space = vspace.value().reference();
    auto capability_space = cspace.value().reference();
    if (!home || !address_space || !capability_space) {
        return returned(MYOS_STATUS_NO_MEMORY);
    }
    auto execution = kernel::ExecutionBinding::user(
        libk::move(address_space).value(),
        libk::move(capability_space).value());
    if (!execution) {
        return returned(MYOS_STATUS_BUSY);
    }

    const auto total = add_budget(
        kernel::resource::Traits<kernel::Thread>::fixed(),
        kernel::resource::Budget{
            .memory = kernel::mm::KernelStackLayout::StackBytes});
    KASSERT(total);
    return construct_with<kernel::Thread>(
        invocation,
        pool.value(),
        kernel::resource::Traits<kernel::Thread>::fixed(),
        *total,
        [&](kernel::resource::Reservation&& sponsorship) {
            return kernel->objects().create_thread_sponsored(
                libk::move(sponsorship),
                libk::move(stack_capacity).commit(),
                libk::move(home).value(),
                libk::move(execution).value(),
                start.value());
        },
        [&](kernel::Thread& thread) noexcept -> myos_status_t {
            return thread.authorize(vspace.value(), cspace.value())
                ? MYOS_STATUS_OK
                : MYOS_STATUS_BUSY;
        },
        [&](kernel::Thread&) {
            const auto rights = basic_rights();
            return PublishedAuthority{
                cap::GrantCeiling{rights}, cap::CapView{rights}};
        });
}

[[gnu::noinline]] [[nodiscard]] auto create_notification(
    Invocation& invocation) noexcept -> Result {
    KernelState* const kernel = invocation.cpu.runtime().kernel;
    KASSERT(kernel != nullptr);
    auto pool = resolve_pool(invocation, cap::Right::Create);
    const u64 badge = invocation.trap.arg(1);
    if (!pool) {
        return returned(cap_status(pool.error()));
    }
    if (badge == 0) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    return construct<kernel::ipc::Notification>(
        invocation,
        pool.value(),
        kernel::resource::Traits<kernel::ipc::Notification>::fixed(),
        kernel::resource::Traits<kernel::ipc::Notification>::fixed(),
        [&](kernel::resource::Reservation&& sponsorship) {
            return kernel->objects().create_notification_sponsored(
                libk::move(sponsorship));
        },
        [&](kernel::ipc::Notification&) {
            const auto rights = cap::Rights::of(
                cap::Right::Duplicate,
                cap::Right::Delegate,
                cap::Right::Inspect,
                cap::Right::Signal,
                cap::Right::Wait,
                cap::Right::Bind,
                cap::Right::Destroy,
                cap::Right::Revoke);
            const cap::NotificationAuthority authority{badge};
            return PublishedAuthority{
                cap::GrantCeiling{rights, authority},
                cap::CapView{rights, authority}};
        });
}

[[gnu::noinline]] [[nodiscard]] auto create_tunnel(
    Invocation& invocation) noexcept -> Result {
    KernelState* const kernel = invocation.cpu.runtime().kernel;
    KASSERT(kernel != nullptr);
    auto pool = resolve_pool(invocation, cap::Right::Create);
    auto source = invocation.cspace.resolve<kernel::Vproc>(
        handle_of(invocation.trap.arg(1)), cap::Rights::of(cap::Right::Bind));
    auto target = invocation.cspace.resolve<kernel::Vproc>(
        handle_of(invocation.trap.arg(2)), cap::Rights::of(cap::Right::Bind));
    const usize slot = invocation.trap.arg(3);
    const usize tag = invocation.trap.arg(4);
    if (!pool || !source || !target) {
        const cap::CSpaceError error = !pool
            ? pool.error()
            : !source ? source.error() : target.error();
        return returned(cap_status(error));
    }
    if (&source.value().object() == &target.value().object()
        || slot >= MYOS_VPROC_MAX_INGRESS
        || invocation.trap.arg(5) != MYOS_TUNNEL_FLAGS_NONE) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    return construct_with<kernel::ipc::Tunnel>(
        invocation,
        pool.value(),
        kernel::resource::Traits<kernel::ipc::Tunnel>::fixed(),
        kernel::resource::Traits<kernel::ipc::Tunnel>::fixed(),
        [&](kernel::resource::Reservation&& sponsorship) {
            return kernel->objects().create_tunnel_sponsored(
                libk::move(sponsorship),
                kernel->cpus(),
                source.value().object(),
                target.value().object(),
                slot,
                tag);
        },
        [&](kernel::ipc::Tunnel& tunnel) noexcept -> myos_status_t {
            return tunnel.authorize(source.value(), target.value())
                ? MYOS_STATUS_OK
                : MYOS_STATUS_BUSY;
        },
        [&](kernel::ipc::Tunnel&) {
            const auto rights = cap::Rights::of(
                cap::Right::Duplicate,
                cap::Right::Delegate,
                cap::Right::Inspect,
                cap::Right::Signal,
                cap::Right::Wait,
                cap::Right::Close,
                cap::Right::Destroy,
                cap::Right::Revoke);
            return PublishedAuthority{
                cap::GrantCeiling{rights}, cap::CapView{rights}};
        });
}

[[gnu::noinline]] [[nodiscard]] auto publish_vproc(
    Invocation& invocation,
    KernelState& kernel,
    cap::Resolved<kernel::resource::ResourcePool>& pool,
    cap::Resolved<kernel::mm::VSpace>& vspace,
    cap::Resolved<cap::CSpace>& cspace,
    cap::Resolved<kernel::mm::MemoryObject>& control,
    cap::Resolved<kernel::mm::MemoryObject>& events,
    arch::UserStart runtime_entry,
    kernel::VprocRuntime&& runtime) noexcept -> Result {
    auto stack_reservation = reserve(pool, kernel::resource::Budget{
        .memory = kernel::mm::KernelStackLayout::StackBytes});
    if (!stack_reservation) {
        return returned(pool_error(stack_reservation.error()));
    }
    auto stack_capacity = libk::move(stack_reservation).value();
    auto home = kernel::KernelStack::create(kernel.kernel_vspace());
    auto address_space = vspace.reference();
    auto capability_space = cspace.reference();
    if (!home || !address_space || !capability_space) {
        return returned(MYOS_STATUS_NO_MEMORY);
    }
    auto execution = kernel::ExecutionBinding::user(
        libk::move(address_space).value(),
        libk::move(capability_space).value());
    if (!execution) {
        return returned(MYOS_STATUS_BUSY);
    }

    const auto total = add_budget(
        kernel::resource::Traits<kernel::Vproc>::fixed(),
        kernel::resource::Budget{
            .memory = kernel::mm::KernelStackLayout::StackBytes});
    KASSERT(total);
    return construct_with<kernel::Vproc>(
        invocation,
        pool,
        kernel::resource::Traits<kernel::Vproc>::fixed(),
        *total,
        [&](kernel::resource::Reservation&& sponsorship) {
            return kernel.objects().create_vproc_sponsored(
                libk::move(sponsorship),
                libk::move(stack_capacity).commit(),
                libk::move(home).value(),
                libk::move(execution).value(),
                runtime_entry,
                libk::move(runtime));
        },
        [&](kernel::Vproc& vproc) noexcept -> myos_status_t {
            return vproc.authorize(vspace, cspace, control, events)
                ? MYOS_STATUS_OK
                : MYOS_STATUS_BUSY;
        },
        [&](kernel::Vproc&) {
            const auto rights = vproc_rights();
            return PublishedAuthority{
                cap::GrantCeiling{rights}, cap::CapView{rights}};
        });
}

[[gnu::noinline]] [[nodiscard]] auto create_vproc(
    Invocation& invocation) noexcept -> Result {
    KernelState* const kernel = invocation.cpu.runtime().kernel;
    KASSERT(kernel != nullptr);
    arch::TrapContext& trap = invocation.trap;
    auto pool = resolve_pool(invocation, cap::Right::Create);
    auto vspace = invocation.cspace.resolve<kernel::mm::VSpace>(
        handle_of(trap.arg(1)), cap::Rights::of(cap::Right::Manage));
    auto cspace = invocation.cspace.resolve<cap::CSpace>(
        handle_of(trap.arg(2)), cap::Rights::of(cap::Right::Manage));
    if (!pool || !vspace || !cspace) {
        const cap::CSpaceError error = !pool
            ? pool.error()
            : !vspace ? vspace.error()
            : cspace.error();
        return returned(cap_status(error));
    }

    auto snapshot = read_snapshot<myos_vproc_start>(
        invocation, handle_of(trap.arg(3)), trap.arg(4));
    if (!snapshot) {
        return returned(snapshot.error());
    }
    const myos_vproc_start& descriptor = snapshot.value();
    arch::UserStart runtime_entry{
        .entry = kernel::mm::VirtAddr{descriptor.entry},
        .stack = kernel::mm::VirtAddr{descriptor.stack},
    };
    for (usize index = 0; index < 6; ++index) {
        runtime_entry.arguments[index] = descriptor.arguments[index];
    }
    const kernel::mm::VirtAddr control_address{descriptor.control_address};
    const kernel::mm::VirtAddr event_address{descriptor.event_address};
    if (descriptor.version != MYOS_VPROC_START_VERSION
        || descriptor.flags != 0 || trap.arg(5) != 0
        || !arch::valid_user_start(runtime_entry)
        || !control_address.valid() || !event_address.valid()
        || !control_address.is_aligned(kernel::mm::page_size)
        || !event_address.is_aligned(kernel::mm::page_size)
        || control_address == event_address) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }

    auto control = invocation.cspace.resolve<kernel::mm::MemoryObject>(
        handle_of(descriptor.control_memory),
        cap::Rights::of(cap::Right::Manage));
    auto events = invocation.cspace.resolve<kernel::mm::MemoryObject>(
        handle_of(descriptor.event_memory),
        cap::Rights::of(cap::Right::Manage));
    if (!control || !events) {
        return returned(cap_status(!control ? control.error() : events.error()));
    }
    auto runtime = prepare_vproc_runtime(
        *kernel, vspace.value(), control.value(), events.value(), descriptor);
    if (!runtime) {
        return returned(runtime.error());
    }

    return publish_vproc(
        invocation,
        *kernel,
        pool.value(),
        vspace.value(),
        cspace.value(),
        control.value(),
        events.value(),
        runtime_entry,
        libk::move(runtime).value());
}

} // namespace

auto handle_construction(
    usize operation,
    Invocation& invocation) noexcept -> Result {
    switch (operation) {
    case MYOS_SYS_RESOURCE_CREATE_CHILD:
        return create_child(invocation);
    case MYOS_SYS_MEMORY_CREATE:
        return create_memory(invocation);
    case MYOS_SYS_VSPACE_CREATE:
        return create_vspace(invocation);
    case MYOS_SYS_CSPACE_CREATE:
        return create_cspace(invocation);
    case MYOS_SYS_SC_CREATE:
        return create_sc(invocation);
    case MYOS_SYS_THREAD_CREATE:
        return create_thread(invocation);
    case MYOS_SYS_NOTIFICATION_CREATE:
        return create_notification(invocation);
    case MYOS_SYS_VPROC_CREATE:
        return create_vproc(invocation);
    case MYOS_SYS_TUNNEL_CREATE:
        return create_tunnel(invocation);
    default:
        break;
    }
    return returned(MYOS_STATUS_INVALID_OP);
}

} // namespace kernel::syscall
