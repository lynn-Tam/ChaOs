#include "kernel/syscall/internal.hpp"

#include <cap/policy.hpp>
#include <core/kernel_state.hpp>
#include <cpu/cpu_local.hpp>
#include <cpu/cpu_runtime.hpp>
#include <execution/vproc.hpp>
#include <libk/checked_arithmetic.hpp>
#include <libk/inplace_vector.hpp>
#include <libk/utility.hpp>
#include <ipc/endpoint.hpp>
#include <ipc/notification.hpp>
#include <ipc/tunnel.hpp>
#include <mm/virtual_layout.hpp>
#include <mm/kernel_stack.hpp>
#include <mm/memory_object.hpp>
#include <mm/vspace.hpp>
#include <object/object_store.hpp>
#include <resource/traits.hpp>
#include <sched/context.hpp>
#include <sched/binding.hpp>
#include <sched/domain.hpp>
#include <thread/thread.hpp>
#include <uapi/syscall.h>
#include <uapi/thread.h>
#include <uapi/tunnel.h>
#include <uapi/vproc.h>
#include <uapi/resource.h>
#include <uapi/endpoint.h>

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
static_assert(MYOS_RESOURCE_ENDPOINT
    == (u64{1} << static_cast<u16>(object::ObjectKind::Endpoint)));

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
        cap::Right::Destroy,
        cap::Right::Revoke);
}

[[nodiscard]] auto memory_allows(
    const cap::Resolved<kernel::mm::MemoryObject>& memory,
    kernel::mm::ObjectRange range,
    kernel::mm::AccessMask access) noexcept -> bool {
    const cap::EffectiveAuthority effective = memory.authority();
    const auto* const authority = libk::get_if<cap::MemoryAuthority>(
        &effective.data);
    return authority != nullptr && authority->range.contains(range)
        && authority->access.contains(access)
        && authority->types.contains(kernel::mm::MemoryType::Normal);
}

[[nodiscard]] auto ipc_error(kernel::ipc::BufferError error) noexcept
    -> myos_status_t {
    switch (error) {
    case kernel::ipc::BufferError::Invalid:
        return MYOS_STATUS_BAD_ARGS;
    case kernel::ipc::BufferError::Unavailable:
        return MYOS_STATUS_BUSY;
    case kernel::ipc::BufferError::NoMemory:
        return MYOS_STATUS_NO_MEMORY;
    }
    return MYOS_STATUS_INTERNAL;
}

[[nodiscard]] auto bind_ipc(
    KernelState& kernel,
    cap::Resolved<kernel::mm::VSpace>& vspace,
    cap::Resolved<kernel::mm::MemoryObject>& memory,
    const myos_ipc_binding& descriptor) noexcept
    -> libk::Expected<kernel::ipc::Buffer, myos_status_t> {
    if (descriptor.pages == 0
        || descriptor.pages > MYOS_IPC_BUFFER_MAX_PAGES) {
        return libk::unexpected(MYOS_STATUS_BAD_ARGS);
    }
    const auto bytes = libk::checked_multiply(
        descriptor.pages, kernel::mm::page_size);
    const kernel::mm::VirtAddr address{descriptor.address};
    if (!bytes || !address.is_aligned(kernel::mm::page_size)) {
        return libk::unexpected(MYOS_STATUS_BAD_ARGS);
    }
    const auto rw = kernel::mm::AccessMask::of(
        kernel::mm::Access::Read, kernel::mm::Access::Write);
    const kernel::mm::ObjectRange object{
        descriptor.page, descriptor.pages};
    if (!memory_allows(memory, object, rw)) {
        return libk::unexpected(MYOS_STATUS_DENIED);
    }
    auto reference = memory.reference();
    if (!reference) {
        return libk::unexpected(MYOS_STATUS_BUSY);
    }
    auto buffer = kernel::ipc::Buffer::bind(
        kernel.pmm(),
        vspace.object(),
        libk::move(reference).value(),
        memory.object(),
        object,
        kernel::mm::VirtRange{address, *bytes});
    return buffer
        ? libk::Expected<kernel::ipc::Buffer, myos_status_t>{
              libk::expected(libk::move(buffer).value())}
        : libk::unexpected(ipc_error(buffer.error()));
}

[[nodiscard]] auto prepare_ipc(
    Invocation& invocation,
    KernelState& kernel,
    cap::Resolved<kernel::mm::VSpace>& vspace,
    const myos_ipc_binding& descriptor) noexcept
    -> libk::Expected<libk::optional<kernel::ipc::Buffer>, myos_status_t> {
    if (descriptor.pages == 0) {
        if (descriptor.memory != 0 || descriptor.page != 0
            || descriptor.address != 0) {
            return libk::unexpected(MYOS_STATUS_BAD_ARGS);
        }
        return libk::expected(
            libk::optional<kernel::ipc::Buffer>{libk::nullopt});
    }
    auto memory = invocation.cspace.resolve<kernel::mm::MemoryObject>(
        handle_of(descriptor.memory), cap::Rights::of(cap::Right::Map));
    if (!memory) {
        return libk::unexpected(cap_status(memory.error()));
    }
    auto buffer = bind_ipc(kernel, vspace, memory.value(), descriptor);
    if (!buffer) {
        return libk::unexpected(buffer.error());
    }
    return libk::expected(libk::optional<kernel::ipc::Buffer>{
        libk::move(buffer).value()});
}

[[gnu::noinline]] [[nodiscard]] auto add_endpoint_slot(
    kernel::ipc::Endpoint& endpoint,
    KernelState& kernel,
    cap::Resolved<kernel::resource::ResourcePool>& pool,
    cap::Resolved<kernel::mm::VSpace>& vspace,
    cap::Resolved<kernel::mm::MemoryObject>& stack,
    cap::Resolved<kernel::mm::MemoryObject>* ipc_memory,
    const myos_endpoint_desc& descriptor,
    usize stack_bytes,
    usize index) noexcept -> myos_status_t {
    auto capacity = reserve(pool, kernel::resource::Budget{
        .memory = kernel::mm::KernelStackLayout::StackBytes});
    auto kernel_stack = KernelStack::create(kernel.kernel_vspace());
    const auto displacement = libk::checked_multiply(
        index, descriptor.stack_stride);
    const auto object_page = libk::checked_multiply(
        index, descriptor.stack_pages);
    if (!capacity || !kernel_stack || !displacement || !object_page) {
        return !capacity
            ? pool_error(capacity.error()) : MYOS_STATUS_NO_MEMORY;
    }
    const auto virtual_base = kernel::mm::VirtAddr{
        descriptor.stack_address}.checked_add(*displacement);
    const auto first_page = libk::checked_add(
        descriptor.stack_page, *object_page);
    if (!virtual_base || !first_page) {
        return MYOS_STATUS_BAD_ARGS;
    }
    auto stack_ref = stack.reference();
    if (!stack_ref) {
        return MYOS_STATUS_BUSY;
    }
    const auto rw = kernel::mm::AccessMask::of(
        kernel::mm::Access::Read, kernel::mm::Access::Write);
    auto user_stack = vspace->bind_view(kernel::mm::UserViewRequest{
        .memory = libk::move(stack_ref).value(),
        .object = kernel::mm::ObjectRange{
            *first_page, descriptor.stack_pages},
        .virtual_range = kernel::mm::VirtRange{*virtual_base, stack_bytes},
        .access = rw,
    });
    if (!user_stack) {
        return vm_status(user_stack.error());
    }
    kernel::ipc::StackPages resident{};
    for (usize page = 0; page < descriptor.stack_pages; ++page) {
        auto lease = stack->materialize(*first_page + page);
        if (!lease || !resident.try_push_back(libk::move(lease).value())) {
            return !lease
                ? MYOS_STATUS_BACKING_FAILED : MYOS_STATUS_NO_MEMORY;
        }
    }
    const auto top = virtual_base->checked_add(stack_bytes);
    if (!top) {
        return MYOS_STATUS_BAD_ARGS;
    }
    libk::optional<kernel::ipc::Buffer> ipc{};
    if (ipc_memory != nullptr) {
        const auto ipc_displacement = libk::checked_multiply(
            index, descriptor.ipc_stride);
        const auto ipc_page = libk::checked_multiply(
            index, descriptor.ipc.pages);
        const auto ipc_address = ipc_displacement
            ? kernel::mm::VirtAddr{descriptor.ipc.address}.checked_add(
                  *ipc_displacement)
            : libk::nullopt;
        const auto first_ipc_page = ipc_page
            ? libk::checked_add(descriptor.ipc.page, *ipc_page)
            : libk::nullopt;
        if (!ipc_address || !first_ipc_page) {
            return MYOS_STATUS_BAD_ARGS;
        }
        myos_ipc_binding binding = descriptor.ipc;
        binding.address = ipc_address->raw();
        binding.page = *first_ipc_page;
        auto made = bind_ipc(kernel, vspace, *ipc_memory, binding);
        if (!made) {
            return made.error();
        }
        ipc.emplace(libk::move(made).value());
    }
    auto added = endpoint.add_activation(
        libk::move(capacity).value().commit(),
        libk::move(kernel_stack).value(),
        libk::move(user_stack).value(),
        libk::move(ipc),
        libk::move(resident),
        *top);
    return added ? MYOS_STATUS_OK : MYOS_STATUS_BAD_ARGS;
}

[[gnu::noinline]] [[nodiscard]] auto finish_endpoint(
    Invocation& invocation,
    KernelState& kernel,
    cap::Resolved<kernel::resource::ResourcePool>& pool,
    cap::Resolved<kernel::mm::VSpace>& vspace,
    cap::Resolved<kernel::mm::MemoryObject>& stack,
    cap::Resolved<kernel::mm::MemoryObject>* ipc_memory,
    const myos_endpoint_desc& descriptor,
    usize stack_bytes,
    ExecutionBinding&& service,
    kernel::mm::UserView&& code_view,
    kernel::ipc::CodePages&& resident_code) noexcept -> Result {
    const auto stack_capacity = libk::checked_multiply(
        descriptor.activation_count,
        kernel::mm::KernelStackLayout::StackBytes);
    const auto call_capacity = libk::checked_add(
        descriptor.activation_count, descriptor.queue_capacity);
    const auto node_capacity = libk::checked_multiply(
        call_capacity ? descriptor.activation_count + *call_capacity : 0,
        kernel::mm::page_size);
    const auto dynamic_capacity = stack_capacity && call_capacity
        && node_capacity
        ? add_budget(
              kernel::resource::Budget{.memory = *stack_capacity},
              kernel::resource::Budget{.memory = *node_capacity})
        : libk::nullopt;
    const auto total_charge = dynamic_capacity
        ? add_budget(
              kernel::resource::Traits<kernel::ipc::Endpoint>::fixed(),
              *dynamic_capacity)
        : libk::nullopt;
    const auto stack_top = kernel::mm::VirtAddr{
        descriptor.stack_address}.checked_add(stack_bytes);
    if (!total_charge || !stack_top) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    const auto budget_floor = kernel.clock().duration_from_nanoseconds(
        descriptor.budget_floor_ns);
    const auto urgency_ceiling = kernel::sched::Urgency::make(
        descriptor.urgency_ceiling);
    if (!budget_floor || !urgency_ceiling) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    kernel::ipc::EndpointConfig config{
        .entry = arch::UserStart{
            .entry = kernel::mm::VirtAddr{descriptor.entry},
            .stack = *stack_top,
        },
        .capacity = descriptor.activation_count,
        .call_capacity = *call_capacity,
        .max_depth = descriptor.max_depth,
        .budget_floor = *budget_floor,
        .urgency_ceiling = *urgency_ceiling,
    };
    return construct_with<kernel::ipc::Endpoint>(
        invocation,
        pool,
        kernel::resource::Traits<kernel::ipc::Endpoint>::fixed(),
        *total_charge,
        [&](kernel::resource::Reservation&& sponsorship) {
            return kernel.objects().create_endpoint_sponsored(
                libk::move(sponsorship),
                libk::move(service),
                libk::move(code_view),
                libk::move(resident_code),
                config);
        },
        [&](kernel::ipc::Endpoint& endpoint) noexcept -> myos_status_t {
            for (usize index = 0;
                 index < descriptor.activation_count;
                 ++index) {
                const myos_status_t added = add_endpoint_slot(
                    endpoint,
                    kernel,
                    pool,
                    vspace,
                    stack,
                    ipc_memory,
                    descriptor,
                    stack_bytes,
                    index);
                if (added != MYOS_STATUS_OK) {
                    return added;
                }
            }
            for (usize index = 0; index < *call_capacity; ++index) {
                if (!endpoint.add_call()) {
                    return MYOS_STATUS_NO_MEMORY;
                }
            }
            return endpoint.open() ? MYOS_STATUS_OK : MYOS_STATUS_BAD_ARGS;
        },
        [](kernel::ipc::Endpoint&) {
            const auto rights = cap::Rights::of(
                cap::Right::Duplicate,
                cap::Right::Delegate,
                cap::Right::Inspect,
                cap::Right::Call,
                cap::Right::Close,
                cap::Right::Destroy,
                cap::Right::Revoke);
            const cap::EndpointAuthority authority{
                .badge = 0,
                .fixed = 0,
                .cap_limit = MYOS_ENDPOINT_MAX_CAPS,
            };
            return PublishedAuthority{
                cap::GrantCeiling{rights, authority},
                cap::CapView{rights, authority}};
        });
}

[[gnu::noinline]] [[nodiscard]] auto publish_endpoint(
    Invocation& invocation,
    KernelState& kernel,
    cap::Resolved<kernel::resource::ResourcePool>& pool,
    cap::Resolved<kernel::mm::VSpace>& vspace,
    cap::Resolved<cap::CSpace>& cspace,
    const myos_endpoint_desc& descriptor) noexcept -> Result {
    if (descriptor.version != MYOS_ENDPOINT_VERSION
        || descriptor.flags != MYOS_ENDPOINT_FLAGS_NONE
        || descriptor.activation_count == 0
        || descriptor.activation_count > MYOS_ENDPOINT_MAX_ACTIVATIONS
        || descriptor.queue_capacity
            > MYOS_ENDPOINT_MAX_CALLS - descriptor.activation_count
        || descriptor.code_pages == 0
        || descriptor.code_pages > MYOS_ENDPOINT_MAX_CODE_PAGES
        || descriptor.stack_pages == 0
        || descriptor.stack_pages > MYOS_ENDPOINT_MAX_STACK_PAGES
        || descriptor.max_depth == 0
        || descriptor.max_depth > MYOS_ENDPOINT_MAX_DEPTH
        || descriptor.urgency_ceiling >= kernel::sched::Urgency::level_count
        || descriptor.stack_stride % kernel::mm::page_size != 0
        ) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    const auto code_bytes = libk::checked_multiply(
        descriptor.code_pages, kernel::mm::page_size);
    const auto stack_bytes = libk::checked_multiply(
        descriptor.stack_pages, kernel::mm::page_size);
    const auto stack_span = libk::checked_multiply(
        descriptor.activation_count - 1, descriptor.stack_stride);
    const auto last_end = stack_span && stack_bytes
        ? libk::checked_add(*stack_span, *stack_bytes)
        : libk::nullopt;
    const bool has_ipc = descriptor.ipc.pages != 0;
    const bool empty_ipc = descriptor.ipc.memory == 0
        && descriptor.ipc.page == 0 && descriptor.ipc.address == 0
        && descriptor.ipc.pages == 0 && descriptor.ipc_stride == 0;
    const auto ipc_bytes = has_ipc
        ? libk::checked_multiply(
              descriptor.ipc.pages, kernel::mm::page_size)
        : libk::optional<usize>{};
    const auto ipc_span = has_ipc
        ? libk::checked_multiply(
              descriptor.activation_count - 1, descriptor.ipc_stride)
        : libk::optional<usize>{};
    const auto ipc_end = has_ipc && ipc_span && ipc_bytes
        ? libk::checked_add(*ipc_span, *ipc_bytes)
        : libk::optional<usize>{};
    if (!code_bytes || !stack_bytes || !last_end
        || descriptor.stack_stride < *stack_bytes) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    if ((!has_ipc && !empty_ipc)
        || (has_ipc && (descriptor.ipc.memory == 0
            || descriptor.ipc.pages > MYOS_IPC_BUFFER_MAX_PAGES
            || !ipc_bytes || !ipc_span || !ipc_end
            || descriptor.ipc_stride < *ipc_bytes
            || descriptor.ipc_stride % kernel::mm::page_size != 0))) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    const kernel::mm::VirtAddr code_base{descriptor.code_address};
    const kernel::mm::VirtAddr stack_base{descriptor.stack_address};
    const kernel::mm::VirtRange code_range{code_base, *code_bytes};
    const kernel::mm::VirtRange stack_extent{stack_base, *last_end};
    const kernel::mm::VirtAddr ipc_base{descriptor.ipc.address};
    const kernel::mm::VirtRange ipc_extent{
        ipc_base, ipc_end ? *ipc_end : 0};
    if (!code_base.is_aligned(kernel::mm::page_size)
        || !stack_base.is_aligned(kernel::mm::page_size)
        || !code_range.valid() || !stack_extent.valid()
        || code_range.intersects(stack_extent)
        || (has_ipc && (!ipc_base.is_aligned(kernel::mm::page_size)
            || !ipc_extent.valid() || ipc_extent.empty()
            || ipc_extent.intersects(code_range)
            || ipc_extent.intersects(stack_extent)))
        || !code_range.contains(kernel::mm::VirtAddr{descriptor.entry})) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }

    auto code = invocation.cspace.resolve<kernel::mm::MemoryObject>(
        handle_of(descriptor.code_memory),
        cap::Rights::of(cap::Right::Map));
    auto stack = invocation.cspace.resolve<kernel::mm::MemoryObject>(
        handle_of(descriptor.stack_memory),
        cap::Rights::of(cap::Right::Map));
    if (!code || !stack) {
        return returned(cap_status(!code ? code.error() : stack.error()));
    }
    libk::optional<cap::Resolved<kernel::mm::MemoryObject>> ipc_memory{};
    if (has_ipc) {
        auto resolved = invocation.cspace.resolve<kernel::mm::MemoryObject>(
            handle_of(descriptor.ipc.memory),
            cap::Rights::of(cap::Right::Map));
        if (!resolved) {
            return returned(cap_status(resolved.error()));
        }
        ipc_memory.emplace(libk::move(resolved).value());
    }
    const auto rx = kernel::mm::AccessMask::of(
        kernel::mm::Access::Read, kernel::mm::Access::Execute);
    const auto rw = kernel::mm::AccessMask::of(
        kernel::mm::Access::Read, kernel::mm::Access::Write);
    const kernel::mm::ObjectRange code_object{
        descriptor.code_page, descriptor.code_pages};
    const auto total_stack_pages = libk::checked_multiply(
        descriptor.activation_count, descriptor.stack_pages);
    const auto total_ipc_pages = has_ipc
        ? libk::checked_multiply(
              descriptor.activation_count, descriptor.ipc.pages)
        : libk::optional<usize>{};
    if (!total_stack_pages || !memory_allows(code.value(), code_object, rx)
        || !memory_allows(
            stack.value(),
            kernel::mm::ObjectRange{
                descriptor.stack_page, *total_stack_pages},
            rw)
        || (has_ipc && (!total_ipc_pages
            || !memory_allows(
                *ipc_memory,
                kernel::mm::ObjectRange{
                    descriptor.ipc.page, *total_ipc_pages},
                rw)))) {
        return returned(MYOS_STATUS_DENIED);
    }

    auto code_ref = code.value().reference();
    if (!code_ref) {
        return returned(MYOS_STATUS_BUSY);
    }
    auto code_view = vspace->bind_view(kernel::mm::UserViewRequest{
        .memory = libk::move(code_ref).value(),
        .object = code_object,
        .virtual_range = code_range,
        .access = rx,
    });
    if (!code_view) {
        return returned(vm_status(code_view.error()));
    }
    kernel::ipc::CodePages resident_code{};
    for (usize page = 0; page < descriptor.code_pages; ++page) {
        auto lease = code.value()->materialize(descriptor.code_page + page);
        if (!lease || !resident_code.try_push_back(libk::move(lease).value())) {
            return returned(!lease
                ? MYOS_STATUS_BACKING_FAILED : MYOS_STATUS_NO_MEMORY);
        }
    }

    auto vspace_ref = vspace.reference();
    auto cspace_ref = cspace.reference();
    if (!vspace_ref || !cspace_ref) {
        return returned(MYOS_STATUS_BUSY);
    }
    auto service = ExecutionBinding::user(
        libk::move(vspace_ref).value(), libk::move(cspace_ref).value());
    if (!service) {
        return returned(MYOS_STATUS_BUSY);
    }

    return finish_endpoint(
        invocation,
        kernel,
        pool,
        vspace,
        stack.value(),
        ipc_memory ? &*ipc_memory : nullptr,
        descriptor,
        *stack_bytes,
        libk::move(service).value(),
        libk::move(code_view).value(),
        libk::move(resident_code));
}

[[gnu::noinline]] [[nodiscard]] auto create_endpoint(
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
            : !vspace ? vspace.error() : cspace.error();
        return returned(cap_status(error));
    }
    auto snapshot = read_snapshot<myos_endpoint_desc>(
        invocation, handle_of(trap.arg(3)), trap.arg(4));
    return snapshot
        ? publish_endpoint(
              invocation,
              *kernel,
              pool.value(),
              vspace.value(),
              cspace.value(),
              snapshot.value())
        : returned(snapshot.error());
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

struct ThreadStart final {
    arch::UserStart user{};
    myos_ipc_binding ipc{};
};

[[nodiscard]] auto start_snapshot(
    Invocation& invocation,
    cap::CapHandle handle,
    usize offset) noexcept -> libk::Expected<ThreadStart, myos_status_t> {
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
    return libk::expected(ThreadStart{start, descriptor.ipc});
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

[[gnu::noinline]] [[nodiscard]] auto publish_thread(
    Invocation& invocation,
    KernelState& kernel,
    cap::Resolved<kernel::resource::ResourcePool>& pool,
    cap::Resolved<kernel::mm::VSpace>& vspace,
    cap::Resolved<cap::CSpace>& cspace,
    ThreadStart&& start,
    libk::optional<kernel::ipc::Buffer>&& ipc) noexcept -> Result {
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
        libk::move(capability_space).value(),
        kernel::FaultRoute::Terminate,
        libk::move(ipc));
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
        pool,
        kernel::resource::Traits<kernel::Thread>::fixed(),
        *total,
        [&](kernel::resource::Reservation&& sponsorship) {
            return kernel.objects().create_thread_sponsored(
                libk::move(sponsorship),
                libk::move(stack_capacity).commit(),
                libk::move(home).value(),
                libk::move(execution).value(),
                start.user);
        },
        [&](kernel::Thread& thread) noexcept -> myos_status_t {
            return thread.authorize(vspace, cspace)
                ? MYOS_STATUS_OK
                : MYOS_STATUS_BUSY;
        },
        [&](kernel::Thread&) {
            const auto rights = basic_rights();
            return PublishedAuthority{
                cap::GrantCeiling{rights}, cap::CapView{rights}};
        });
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
    auto ipc = prepare_ipc(
        invocation, *kernel, vspace.value(), start.value().ipc);
    if (!ipc) {
        return returned(ipc.error());
    }
    return publish_thread(
        invocation,
        *kernel,
        pool.value(),
        vspace.value(),
        cspace.value(),
        libk::move(start).value(),
        libk::move(ipc).value());
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
                cap::Right::Receive,
                cap::Right::Destroy,
                cap::Right::Revoke);
            const cap::NotificationAuthority authority{badge};
            return PublishedAuthority{
                cap::GrantCeiling{rights, authority},
                cap::CapView{rights, authority}};
        });
}

[[gnu::noinline]] [[nodiscard]] auto open_tunnel(
    Invocation& invocation) noexcept -> Result {
    KernelState* const kernel = invocation.cpu.runtime().kernel;
    KASSERT(kernel != nullptr);
    Vproc* const target = invocation.target.vproc();
    if (target == nullptr) {
        return returned(MYOS_STATUS_DENIED);
    }
    auto pool = resolve_pool(invocation, cap::Right::Create);
    if (!pool) {
        return returned(cap_status(pool.error()));
    }
    const usize slot = invocation.trap.arg(1);
    const usize tag = invocation.trap.arg(2);
    if (slot >= MYOS_VPROC_MAX_INGRESS
        || invocation.trap.arg(3) != MYOS_TUNNEL_FLAGS_NONE) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    sched::Binding* const binding = target->binding();
    if (binding == nullptr) {
        return returned(MYOS_STATUS_BUSY);
    }
    auto reference = binding->target_reference();
    if (!reference) {
        return returned(MYOS_STATUS_BUSY);
    }
    auto hold = libk::move(reference).value().into_hold<kernel::Vproc>();
    if (!hold) {
        return returned(MYOS_STATUS_BUSY);
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
                libk::move(hold).value(),
                slot,
                tag);
        },
        [&](kernel::ipc::Tunnel& tunnel) noexcept -> myos_status_t {
            return tunnel.open()
                ? MYOS_STATUS_OK
                : MYOS_STATUS_BUSY;
        },
        [&](kernel::ipc::Tunnel&) {
            const auto rights = cap::Rights::of(
                cap::Right::Duplicate,
                cap::Right::Delegate,
                cap::Right::Inspect,
                cap::Right::Ack,
                cap::Right::Connect,
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
    kernel::VprocRuntime&& runtime,
    libk::optional<kernel::ipc::Buffer>&& ipc) noexcept -> Result {
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
        libk::move(capability_space).value(),
        kernel::FaultRoute::Terminate,
        libk::move(ipc));
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
    auto ipc = prepare_ipc(
        invocation, *kernel, vspace.value(), descriptor.ipc);
    if (!ipc) {
        return returned(ipc.error());
    }
    if (ipc.value()
        && (ipc.value()->virtual_range().contains(control_address)
            || ipc.value()->virtual_range().contains(event_address))) {
        return returned(MYOS_STATUS_BAD_ARGS);
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
        libk::move(runtime).value(),
        libk::move(ipc).value());
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
    case MYOS_SYS_TUNNEL_OPEN:
        return open_tunnel(invocation);
    case MYOS_SYS_ENDPOINT_CREATE:
        return create_endpoint(invocation);
    default:
        break;
    }
    return returned(MYOS_STATUS_INVALID_OP);
}

} // namespace kernel::syscall
