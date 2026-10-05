#include <expected>
#include <array>
#include <optional>
#include <syscall/call.hpp>

#include <cap/cap.hpp>
#include <state.hpp>
#include <cpu/local.hpp>
#include <cpu/runtime.hpp>
#include <libk/checked_arithmetic.hpp>
#include <libk/inplace_vector.hpp>
#include <utility>
#include <ipc/endpoint.hpp>
#include <ipc/channel.hpp>
#include <object/ref.hpp>
#include <uapi/channel.h>
#include <ipc/notification.hpp>
#include <mm/table.hpp>
#include <mm/kspace.hpp>
#include <mm/mem.hpp>
#include <mm/vspace.hpp>
#include <sched/sc.hpp>
#include <sched/domain.hpp>
#include <task/thread.hpp>
#include <uapi/syscall.h>
#include <uapi/thread.h>
#include <uapi/vm.h>
#include <uapi/resource.h>
#include <uapi/endpoint.h>
#include <mm/pager.hpp>
#include <irq/irq.hpp>

namespace syscall {

static_assert(MYOS_RESOURCE_THREAD == (u64{1} << static_cast<u16>(object::ObjectKind::Thread)));
static_assert(MYOS_RESOURCE_SCHED_CONTEXT == (u64{1} << static_cast<u16>(object::ObjectKind::Sc)));
static_assert(MYOS_RESOURCE_CSPACE == (u64{1} << static_cast<u16>(object::ObjectKind::CSpace)));
static_assert(MYOS_RESOURCE_MEMORY == (u64{1} << static_cast<u16>(object::ObjectKind::Mem)));
static_assert(MYOS_RESOURCE_VSPACE == (u64{1} << static_cast<u16>(object::ObjectKind::VSpace)));
static_assert(MYOS_RESOURCE_POOL == (u64{1} << static_cast<u16>(object::ObjectKind::group)));
static_assert(MYOS_RESOURCE_NOTIFICATION == (u64{1} << static_cast<u16>(object::ObjectKind::Notification)));
static_assert(MYOS_RESOURCE_ENDPOINT == (u64{1} << static_cast<u16>(object::ObjectKind::Endpoint)));
static_assert(MYOS_RESOURCE_CHANNEL == (u64{1} << static_cast<u16>(object::ObjectKind::Channel)));
static_assert(MYOS_RESOURCE_PAGER == (u64{1} << static_cast<u16>(object::ObjectKind::Pager)));
static_assert(MYOS_RESOURCE_IRQ == (u64{1} << static_cast<u16>(object::ObjectKind::Irq)));
static_assert(MYOS_RESOURCE_IO_SPACE == (u64{1} << static_cast<u16>(object::ObjectKind::IoSpace)));

using object::ObjectKind;

[[nodiscard]] static constexpr auto kind_bit(ObjectKind kind) noexcept -> u64 {
    return u64{1} << static_cast<u16>(kind);
}

[[nodiscard]] static auto pool_quota(const cap::Resolved<object::group>& pool) noexcept
    -> std::optional<cap::Quota> {
    const cap::View effective = pool.view();
    const auto* const lim = std::get_if<cap::Quota>(&effective.data);
    return lim != nullptr ? std::optional<cap::Quota>{*lim} : std::nullopt;
}

[[nodiscard]] static auto add_budget(resource::budget first, resource::budget second) noexcept
    -> std::optional<resource::budget> {
    const auto memory = libk::checked_add(first.memory, second.memory);
    const auto caps = libk::checked_add(first.caps, second.caps);
    return memory && caps ? std::optional<resource::budget>{resource::budget{*memory, *caps}} : std::nullopt;
}

[[nodiscard]] static auto reserve(cap::Resolved<object::group>& pool, resource::budget charge) noexcept
    -> std::expected<resource::Reservation, resource::errc> {
    auto reference = pool.reference();
    if (!reference) {
        return std::unexpected(resource::errc::invalid);
    }
    return pool->reserve(std::move(reference).value(), charge);
}

[[nodiscard]] static auto begin(cap::Resolved<object::group>& pool) noexcept
    -> std::expected<object::group::Txn, resource::errc> {
    auto reference = pool.reference();
    if (!reference) {
        return std::unexpected(resource::errc::invalid);
    }
    return pool->begin(std::move(reference).value());
}

[[nodiscard]] static auto pool_error(resource::errc error) noexcept -> myos_status_t {
    switch (error) {
    case resource::errc::invalid:
        return MYOS_STATUS_INVALID_CAP;
    case resource::errc::closed:
        return MYOS_STATUS_BUSY;
    case resource::errc::exhausted:
        return MYOS_STATUS_NO_MEMORY;
    }
    return MYOS_STATUS_INTERNAL;
}

template <class T, usize N = 1>
static auto begin_create(Call& inv, cap::Resolved<object::group>& pool, resource::budget limit) noexcept
    -> std::expected<object::group::Txn, myos_status_t> {
    auto& kernel = *inv.cpu.runtime().kernel;
    const auto quota = pool_quota(pool);
    std::optional<resource::budget> publication = object::group::allocation_charge();
    for (usize i = 0; i < N && publication; ++i)
        publication = add_budget(*publication, kernel.grants().node_charge());
    if (!quota || !(quota->object_kinds & kind_bit(object::kind<T>)) || !quota->budget.contains(limit) ||
        !publication || !quota->budget.contains(*publication))
        return std::unexpected(MYOS_STATUS_DENIED);
    auto txn = begin(pool);
    if (!txn) return std::unexpected(pool_error(txn.error()));
    return std::move(*txn);
}

template <usize N>
static auto publication(std::expected<std::array<cap::Handle, N>, object::group::Txn::Error> handles) noexcept
    -> Result {
    if (!handles)
        return returned(std::visit(
            [](auto error) -> myos_status_t {
                if constexpr (std::is_same_v<decltype(error), resource::errc>)
                    return pool_error(error);
                else if constexpr (std::is_same_v<decltype(error), cap::CSpaceError>)
                    return cap_status(error);
                else
                    return MYOS_STATUS_NO_MEMORY;
            },
            handles.error()));
    return Result{MYOS_STATUS_OK, (*handles)[0].raw(), Disposition::Return,
                  N > 1 ? (*handles)[N - 1].raw() : 0};
}

[[nodiscard]] static auto resolve_pool(Call& inv, cap::Right right) noexcept
    -> std::expected<cap::Resolved<object::group>, cap::CSpaceError> {
    return inv.cspace.resolve<object::group>(handle_of(inv.trap.arg(0)), cap::Rights::of(right));
}

[[nodiscard]] static constexpr auto basic_rights() noexcept -> cap::Rights {
    return cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect,
                           cap::Right::Control, cap::Right::Observe, cap::Right::Destroy, cap::Right::Revoke);
}

[[nodiscard]] static constexpr auto sc_rights() noexcept -> cap::Rights {
    return cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect,
                           cap::Right::Control, cap::Right::Destroy, cap::Right::Revoke);
}

[[nodiscard]] static auto mem_permits(const cap::Resolved<mm::Mem>& memory, mm::ObjectRange range,
                                        mm::Perms access) noexcept -> bool {
    const cap::View effective = memory.view();
    const auto* const lim = std::get_if<cap::MemLimit>(&effective.data);
    return lim != nullptr && lim->range.contains(range) && lim->access.contains(access) &&
           lim->types.contains(mm::MemoryType::Normal);
}

[[nodiscard]] static auto ipc_error(ipc::BufferError error) noexcept -> myos_status_t {
    switch (error) {
    case ipc::BufferError::Invalid:
        return MYOS_STATUS_BAD_ARGS;
    case ipc::BufferError::Unavailable:
        return MYOS_STATUS_BUSY;
    case ipc::BufferError::NoMemory:
        return MYOS_STATUS_NO_MEMORY;
    }
    return MYOS_STATUS_INTERNAL;
}

[[nodiscard]] static auto bind_ipc(KernelState& kernel, cap::Resolved<mm::VSpace>& vspace,
                                   cap::Resolved<mm::Mem>& memory,
                                   const myos_ipc_binding& desc) noexcept
    -> std::expected<ipc::Buffer, myos_status_t> {
    if (desc.pages == 0 || desc.pages > MYOS_IPC_BUFFER_MAX_PAGES) {
        return std::unexpected(MYOS_STATUS_BAD_ARGS);
    }
    const auto bytes = libk::checked_multiply(desc.pages, mm::page_size);
    const mm::Virt address{desc.address};
    if (!bytes || !address.is_aligned(mm::page_size)) {
        return std::unexpected(MYOS_STATUS_BAD_ARGS);
    }
    const auto rw = mm::Perms::of(mm::Perm::Read, mm::Perm::Write);
    const mm::ObjectRange object{desc.page, desc.pages};
    if (!mem_permits(memory, object, rw)) {
        return std::unexpected(MYOS_STATUS_DENIED);
    }
    auto reference = memory.reference();
    if (!reference) {
        return std::unexpected(MYOS_STATUS_BUSY);
    }
    auto buffer = ipc::Buffer::bind(kernel.pmm(), vspace.object(), std::move(reference).value(),
                                    memory.object(), object, mm::VRange{address, *bytes});
    return buffer ? std::expected<ipc::Buffer, myos_status_t>{(std::move(buffer).value())}
                  : std::unexpected(ipc_error(buffer.error()));
}

[[nodiscard]] static auto prepare_ipc(Call& inv, KernelState& kernel, cap::Resolved<mm::VSpace>& vspace,
                                      const myos_ipc_binding& desc) noexcept
    -> std::expected<std::optional<ipc::Buffer>, myos_status_t> {
    if (desc.pages == 0) {
        if (desc.memory != 0 || desc.page != 0 || desc.address != 0) {
            return std::unexpected(MYOS_STATUS_BAD_ARGS);
        }
        return (std::optional<ipc::Buffer>{std::nullopt});
    }
    auto memory = inv.cspace.resolve<mm::Mem>(handle_of(desc.memory), cap::Rights::of(cap::Right::Map));
    if (!memory) {
        return std::unexpected(cap_status(memory.error()));
    }
    auto buffer = bind_ipc(kernel, vspace, memory.value(), desc);
    if (!buffer) {
        return std::unexpected(buffer.error());
    }
    return (std::optional<ipc::Buffer>{std::move(buffer).value()});
}

[[gnu::noinline]] [[nodiscard]] static auto
add_endpoint_slot(ipc::Endpoint& endpoint, KernelState& kernel, cap::Resolved<object::group>& pool,
                  cap::Resolved<mm::VSpace>& vspace, cap::Resolved<mm::Mem>& stack,
                  cap::Resolved<mm::Mem>* ipc_memory, const myos_endpoint_desc& desc, usize stack_bytes,
                  usize index) noexcept -> myos_status_t {
    auto capacity = reserve(pool, resource::budget{.memory = mm::Stack::StackBytes});
    auto kernel_stack = mm::Stack::create(kernel.kernel_vspace());
    const auto displacement = libk::checked_multiply(index, desc.stack_stride);
    const auto object_page = libk::checked_multiply(index, desc.stack_pages);
    if (!capacity || !kernel_stack || !displacement || !object_page) {
        return !capacity ? pool_error(capacity.error()) : MYOS_STATUS_NO_MEMORY;
    }
    const auto virtual_base = mm::Virt{desc.stack_address}.checked_add(*displacement);
    const auto first_page = libk::checked_add(desc.stack_page, *object_page);
    if (!virtual_base || !first_page) {
        return MYOS_STATUS_BAD_ARGS;
    }
    auto stack_ref = stack.reference();
    if (!stack_ref) {
        return MYOS_STATUS_BUSY;
    }
    const auto rw = mm::Perms::of(mm::Perm::Read, mm::Perm::Write);
    auto user_stack = vspace->bind_view(mm::ViewReq{
        .memory = std::move(stack_ref).value(),
        .object = mm::ObjectRange{*first_page, desc.stack_pages},
        .virtual_range = mm::VRange{*virtual_base, stack_bytes},
        .perms = rw,
    });
    if (!user_stack) {
        return vm_status(user_stack.error());
    }
    ipc::StackPages resident{};
    for (usize page = 0; page < desc.stack_pages; ++page) {
        auto lease = stack->materialize(*first_page + page);
        if (!lease || !resident.try_push_back(std::move(lease).value())) {
            return !lease ? MYOS_STATUS_BACKING_FAILED : MYOS_STATUS_NO_MEMORY;
        }
    }
    const auto top = virtual_base->checked_add(stack_bytes);
    if (!top) {
        return MYOS_STATUS_BAD_ARGS;
    }
    std::optional<ipc::Buffer> ipc{};
    if (ipc_memory != nullptr) {
        const auto ipc_displacement = libk::checked_multiply(index, desc.ipc_stride);
        const auto ipc_page = libk::checked_multiply(index, desc.ipc.pages);
        const auto ipc_address =
            ipc_displacement ? mm::Virt{desc.ipc.address}.checked_add(*ipc_displacement) : std::nullopt;
        const auto first_ipc_page =
            ipc_page ? libk::checked_add(desc.ipc.page, *ipc_page) : std::nullopt;
        if (!ipc_address || !first_ipc_page) {
            return MYOS_STATUS_BAD_ARGS;
        }
        myos_ipc_binding binding = desc.ipc;
        binding.address = ipc_address->raw();
        binding.page = *first_ipc_page;
        auto made = bind_ipc(kernel, vspace, *ipc_memory, binding);
        if (!made) {
            return made.error();
        }
        ipc.emplace(std::move(made).value());
    }
    auto added =
        endpoint.add_activation(std::move(capacity).value().commit(), std::move(kernel_stack).value(),
                                std::move(user_stack).value(), std::move(ipc), std::move(resident), *top);
    return added ? MYOS_STATUS_OK : MYOS_STATUS_BAD_ARGS;
}

[[gnu::noinline]] [[nodiscard]] static auto
finish_endpoint(Call& inv, KernelState& kernel, cap::Resolved<object::group>& pool,
                cap::Resolved<mm::VSpace>& vspace, cap::Resolved<mm::Mem>& stack,
                cap::Resolved<mm::Mem>* ipc_memory, const myos_endpoint_desc& desc, usize stack_bytes,
                Env&& service, mm::View&& code_view, ipc::CodePages&& resident_code) noexcept -> Result {
    const auto stack_capacity = libk::checked_multiply(desc.activation_count, mm::Stack::StackBytes);
    const auto call_capacity = libk::checked_add(desc.activation_count, desc.queue_capacity);
    const auto node_capacity = libk::checked_multiply(
        call_capacity ? desc.activation_count + *call_capacity : 0, mm::page_size);
    const auto dynamic_capacity = stack_capacity && call_capacity && node_capacity
                                      ? add_budget(resource::budget{.memory = *stack_capacity},
                                                   resource::budget{.memory = *node_capacity})
                                      : std::nullopt;
    const auto total_charge = dynamic_capacity
                                  ? add_budget(object::pool<ipc::Endpoint>::slot_charge(), *dynamic_capacity)
                                  : std::nullopt;
    const auto stack_top = mm::Virt{desc.stack_address}.checked_add(stack_bytes);
    if (!total_charge || !stack_top) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    const auto budget_floor = kernel.clock().duration_from_nanoseconds(desc.budget_floor_ns);
    const auto urgency_ceiling = sched::Urgency::make(desc.urgency_ceiling);
    if (!budget_floor || !urgency_ceiling) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    ipc::EndpointConfig config{
        .entry =
            arch::UserStart{
                .entry = mm::Virt{desc.entry},
                .stack = *stack_top,
            },
        .capacity = desc.activation_count,
        .call_capacity = *call_capacity,
        .max_depth = desc.max_depth,
        .budget_floor = *budget_floor,
        .urgency_ceiling = *urgency_ceiling,
    };
    auto txn = begin_create<ipc::Endpoint>(inv, pool, *total_charge);
    if (!txn) return returned(txn.error());
    auto fee = reserve(pool, object::pool<ipc::Endpoint>::slot_charge());
    if (!fee) return returned(pool_error(fee.error()));
    auto object = txn->make(kernel.ipc().endpoints, std::move(*fee), kernel.pmm(), std::move(service),
                            std::move(code_view), std::move(resident_code), config);
    if (!object) return returned(MYOS_STATUS_NO_MEMORY);
    auto& obj = object->get();
    const auto rights =
        cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect, cap::Right::Call,
                        cap::Right::Close, cap::Right::Destroy, cap::Right::Revoke);
    const cap::EpLimit lim{
        .badge = 0,
        .fixed = 0,
        .cap_limit = MYOS_ENDPOINT_MAX_CAPS,
    };
    const auto caps = std::array<cap::View, 1>{cap::View{rights, lim}};
    if (!txn->root(kernel.grants(), caps[0])) return returned(MYOS_STATUS_NO_MEMORY);
    for (usize index = 0; index < desc.activation_count; ++index) {
        const myos_status_t added =
            add_endpoint_slot(obj, kernel, pool, vspace, stack, ipc_memory, desc, stack_bytes, index);
        if (added != MYOS_STATUS_OK) {
            return returned(added);
        }
    }
    for (usize index = 0; index < *call_capacity; ++index) {
        if (!obj.add_call()) {
            return returned(MYOS_STATUS_NO_MEMORY);
        }
    }
    if (!obj.open()) return returned(MYOS_STATUS_BAD_ARGS);
    return publication(txn->publish(inv.cspace, caps));
}

[[gnu::noinline]] [[nodiscard]] static auto
publish_endpoint(Call& inv, KernelState& kernel, cap::Resolved<object::group>& pool,
                 cap::Resolved<mm::VSpace>& vspace, cap::Resolved<cap::CSpace>& cspace,
                 const myos_endpoint_desc& desc) noexcept -> Result {
    if (desc.version != MYOS_ENDPOINT_VERSION || desc.flags != MYOS_ENDPOINT_FLAGS_NONE ||
        desc.activation_count == 0 || desc.activation_count > MYOS_ENDPOINT_MAX_ACTIVATIONS ||
        desc.queue_capacity > MYOS_ENDPOINT_MAX_CALLS - desc.activation_count ||
        desc.code_pages == 0 || desc.code_pages > MYOS_ENDPOINT_MAX_CODE_PAGES ||
        desc.stack_pages == 0 || desc.stack_pages > MYOS_ENDPOINT_MAX_STACK_PAGES ||
        desc.max_depth == 0 || desc.max_depth > MYOS_ENDPOINT_MAX_DEPTH ||
        desc.urgency_ceiling >= sched::Urgency::level_count ||
        desc.stack_stride % mm::page_size != 0) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    const auto code_bytes = libk::checked_multiply(desc.code_pages, mm::page_size);
    const auto stack_bytes = libk::checked_multiply(desc.stack_pages, mm::page_size);
    const auto stack_span = libk::checked_multiply(desc.activation_count - 1, desc.stack_stride);
    const auto last_end =
        stack_span && stack_bytes ? libk::checked_add(*stack_span, *stack_bytes) : std::nullopt;
    const bool has_ipc = desc.ipc.pages != 0;
    const bool empty_ipc = desc.ipc.memory == 0 && desc.ipc.page == 0 &&
                           desc.ipc.address == 0 && desc.ipc.pages == 0 &&
                           desc.ipc_stride == 0;
    const auto ipc_bytes =
        has_ipc ? libk::checked_multiply(desc.ipc.pages, mm::page_size) : std::optional<usize>{};
    const auto ipc_span = has_ipc
                              ? libk::checked_multiply(desc.activation_count - 1, desc.ipc_stride)
                              : std::optional<usize>{};
    const auto ipc_end =
        has_ipc && ipc_span && ipc_bytes ? libk::checked_add(*ipc_span, *ipc_bytes) : std::optional<usize>{};
    if (!code_bytes || !stack_bytes || !last_end || desc.stack_stride < *stack_bytes) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    if ((!has_ipc && !empty_ipc) ||
        (has_ipc && (desc.ipc.memory == 0 || desc.ipc.pages > MYOS_IPC_BUFFER_MAX_PAGES ||
                     !ipc_bytes || !ipc_span || !ipc_end || desc.ipc_stride < *ipc_bytes ||
                     desc.ipc_stride % mm::page_size != 0))) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    const mm::Virt code_base{desc.code_address};
    const mm::Virt stack_base{desc.stack_address};
    const mm::VRange code_range{code_base, *code_bytes};
    const mm::VRange stack_extent{stack_base, *last_end};
    const mm::Virt ipc_base{desc.ipc.address};
    const mm::VRange ipc_extent{ipc_base, ipc_end ? *ipc_end : 0};
    if (!code_base.is_aligned(mm::page_size) || !stack_base.is_aligned(mm::page_size) ||
        !code_range.valid() || !stack_extent.valid() || code_range.intersects(stack_extent) ||
        (has_ipc && (!ipc_base.is_aligned(mm::page_size) || !ipc_extent.valid() || ipc_extent.empty() ||
                     ipc_extent.intersects(code_range) || ipc_extent.intersects(stack_extent))) ||
        !code_range.contains(mm::Virt{desc.entry})) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }

    auto code =
        inv.cspace.resolve<mm::Mem>(handle_of(desc.code_memory), cap::Rights::of(cap::Right::Map));
    auto stack =
        inv.cspace.resolve<mm::Mem>(handle_of(desc.stack_memory), cap::Rights::of(cap::Right::Map));
    if (!code || !stack) {
        return returned(cap_status(!code ? code.error() : stack.error()));
    }
    std::optional<cap::Resolved<mm::Mem>> ipc_memory{};
    if (has_ipc) {
        auto resolved =
            inv.cspace.resolve<mm::Mem>(handle_of(desc.ipc.memory), cap::Rights::of(cap::Right::Map));
        if (!resolved) {
            return returned(cap_status(resolved.error()));
        }
        ipc_memory.emplace(std::move(resolved).value());
    }
    const auto rx = mm::Perms::of(mm::Perm::Read, mm::Perm::Execute);
    const auto rw = mm::Perms::of(mm::Perm::Read, mm::Perm::Write);
    const mm::ObjectRange code_object{desc.code_page, desc.code_pages};
    const auto total_stack_pages =
        libk::checked_multiply(desc.activation_count, desc.stack_pages);
    const auto total_ipc_pages =
        has_ipc ? libk::checked_multiply(desc.activation_count, desc.ipc.pages)
                : std::optional<usize>{};
    if (!total_stack_pages || !mem_permits(code.value(), code_object, rx) ||
        !mem_permits(stack.value(), mm::ObjectRange{desc.stack_page, *total_stack_pages}, rw) ||
        (has_ipc &&
         (!total_ipc_pages ||
          !mem_permits(*ipc_memory, mm::ObjectRange{desc.ipc.page, *total_ipc_pages}, rw)))) {
        return returned(MYOS_STATUS_DENIED);
    }

    auto code_ref = code.value().reference();
    if (!code_ref) {
        return returned(MYOS_STATUS_BUSY);
    }
    auto code_view = vspace->bind_view(mm::ViewReq{
        .memory = std::move(code_ref).value(),
        .object = code_object,
        .virtual_range = code_range,
        .perms = rx,
    });
    if (!code_view) {
        return returned(vm_status(code_view.error()));
    }
    ipc::CodePages resident_code{};
    for (usize page = 0; page < desc.code_pages; ++page) {
        auto lease = code.value()->materialize(desc.code_page + page);
        if (!lease || !resident_code.try_push_back(std::move(lease).value())) {
            return returned(!lease ? MYOS_STATUS_BACKING_FAILED : MYOS_STATUS_NO_MEMORY);
        }
    }

    auto vspace_ref = vspace.reference();
    auto cspace_ref = cspace.reference();
    if (!vspace_ref || !cspace_ref) {
        return returned(MYOS_STATUS_BUSY);
    }
    auto service = Env::user(std::move(vspace_ref).value(), std::move(cspace_ref).value());
    if (!service) {
        return returned(MYOS_STATUS_BUSY);
    }

    return finish_endpoint(inv, kernel, pool, vspace, stack.value(), ipc_memory ? &*ipc_memory : nullptr,
                           desc, *stack_bytes, std::move(service).value(), std::move(code_view).value(),
                           std::move(resident_code));
}

template <usize op> [[nodiscard]] auto channel_create(Call& inv) noexcept -> Result {
    KernelState* const kernel = inv.cpu.runtime().kernel;
    libk_assert(kernel != nullptr);
    auto pool = resolve_pool(inv, cap::Right::Create);
    if (!pool) {
        return returned(cap_status(pool.error()));
    }
    const auto config = ipc::ChannelConfig{
        .queue_capacity = inv.trap.arg(1),
        .max_words = inv.trap.arg(2),
        .max_caps = inv.trap.arg(3),
        .relation_capacity = inv.trap.arg(4),
    };
    if (config.queue_capacity == 0 || config.queue_capacity > MYOS_CHANNEL_MAX_QUEUE ||
        config.max_words == 0 || config.max_words > MYOS_CHANNEL_MAX_WORDS ||
        config.max_caps > MYOS_CHANNEL_MAX_CAPS || config.relation_capacity == 0 ||
        config.relation_capacity > MYOS_CHANNEL_MAX_RELATIONS) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    const auto bytes = ipc::Channel::storage_bytes(config);
    const auto cost =
        bytes ? add_budget(object::pool<ipc::Channel>::slot_charge(), resource::budget{.memory = *bytes})
              : std::nullopt;
    if (!cost) return returned(MYOS_STATUS_BAD_ARGS);
    auto txn = begin_create<ipc::Channel, 2>(inv, pool.value(), *cost);
    if (!txn) return returned(txn.error());
    auto fee = reserve(pool.value(), *cost);
    if (!fee) return returned(pool_error(fee.error()));
    auto object = txn->make(kernel->ipc().channels, std::move(*fee), kernel->pmm(), config);
    if (!object) return returned(MYOS_STATUS_NO_MEMORY);
    auto& obj = object->get();
    if (!obj.open()) return returned(MYOS_STATUS_NO_MEMORY);
    const auto rights =
        cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect, cap::Right::Send,
                        cap::Right::Receive, cap::Right::Close, cap::Right::Destroy, cap::Right::Revoke);
    const auto view = [&](cap::ChannelSide side) {
        return cap::View{rights, cap::ChanLimit{.side = side, .badge = 0, .fixed = 0}};
    };
    const auto [ceiling, caps] = std::pair{view(cap::ChannelSide::Any),
                                           std::array{view(cap::ChannelSide::A), view(cap::ChannelSide::B)}};
    if (!txn->root(kernel->grants(), ceiling)) return returned(MYOS_STATUS_NO_MEMORY);
    auto handles = txn->publish(inv.cspace, caps, [&](cap::GrantRef& grant, usize i) {
        return bool(obj.bind_side_root(grant, i == 0 ? cap::ChannelSide::A : cap::ChannelSide::B));
    });
    return publication(std::move(handles));
}

template <usize op> [[gnu::noinline]] [[nodiscard]] auto pager_create(Call& inv) noexcept -> Result {
    KernelState* const kernel = inv.cpu.runtime().kernel;
    libk_assert(kernel != nullptr);
    auto pool = resolve_pool(inv, cap::Right::Create);
    if (!pool) return returned(cap_status(pool.error()));
    auto txn = begin_create<Pager>(inv, pool.value(), object::pool<Pager>::slot_charge());
    if (!txn) return returned(txn.error());
    auto fee = reserve(pool.value(), object::pool<Pager>::slot_charge());
    if (!fee) return returned(pool_error(fee.error()));
    auto object = txn->make(kernel->pool<Pager>(), std::move(*fee));
    if (!object) return returned(MYOS_STATUS_NO_MEMORY);
    const auto rights =
        cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect, cap::Right::Attach,
                        cap::Right::Serve, cap::Right::Supply, cap::Right::Fail, cap::Right::WritebackAck,
                        cap::Right::Close, cap::Right::Destroy, cap::Right::Revoke);
    const auto caps = std::array<cap::View, 1>{cap::View{rights}};
    if (!txn->root(kernel->grants(), caps[0])) return returned(MYOS_STATUS_NO_MEMORY);
    return publication(txn->publish(inv.cspace, caps));
}

template <usize op> [[gnu::noinline]] [[nodiscard]] auto endpoint_create(Call& inv) noexcept -> Result {
    KernelState* const kernel = inv.cpu.runtime().kernel;
    libk_assert(kernel != nullptr);
    arch::TrapContext& trap = inv.trap;
    auto pool = resolve_pool(inv, cap::Right::Create);
    auto vspace = inv.cspace.resolve<mm::VSpace>(handle_of(trap.arg(1)), cap::Rights::of(cap::Right::Manage));
    auto cspace =
        inv.cspace.resolve<cap::CSpace>(handle_of(trap.arg(2)), cap::Rights::of(cap::Right::Manage));
    if (!pool || !vspace || !cspace) {
        const cap::CSpaceError error = !pool ? pool.error() : !vspace ? vspace.error() : cspace.error();
        return returned(cap_status(error));
    }
    auto snapshot = read_desc<myos_endpoint_desc>(inv, handle_of(trap.arg(3)), trap.arg(4));
    return snapshot ? publish_endpoint(inv, *kernel, pool.value(), vspace.value(), cspace.value(),
                                       snapshot.value())
                    : returned(snapshot.error());
}

template <usize op> [[gnu::noinline]] [[nodiscard]] auto resource_create_child(Call& inv) noexcept -> Result {
    KernelState* const kernel = inv.cpu.runtime().kernel;
    libk_assert(kernel != nullptr);
    arch::TrapContext& trap = inv.trap;
    auto pool = resolve_pool(inv, cap::Right::Split);
    if (!pool) {
        return returned(cap_status(pool.error()));
    }
    const resource::budget limit{.memory = trap.arg(1), .caps = trap.arg(2)};
    const u64 kinds = trap.arg(3);
    const auto parent = pool_quota(pool.value());
    if (!parent || !parent->budget.contains(limit) || (kinds & ~parent->object_kinds) != 0 || kinds == 0) {
        return returned(MYOS_STATUS_DENIED);
    }
    const auto charge = add_budget(object::pool<object::group>::slot_charge(), limit);
    if (!charge) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    const auto rights =
        cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect, cap::Right::Create,
                        cap::Right::Split, cap::Right::Close, cap::Right::Revoke);
    auto txn = begin_create<object::group>(inv, pool.value(), limit);
    if (!txn) return returned(txn.error());
    auto fee = reserve(pool.value(), *charge);
    if (!fee) return returned(pool_error(fee.error()));
    auto object = txn->make(kernel->tasks().groups, std::move(*fee), kernel->pmm(), limit);
    if (!object) return returned(MYOS_STATUS_NO_MEMORY);
    const cap::Quota data{limit, kinds};
    const auto caps = std::array<cap::View, 1>{cap::View{rights, data}};
    if (!txn->root(kernel->grants(), caps[0])) return returned(MYOS_STATUS_NO_MEMORY);
    return publication(txn->publish(inv.cspace, caps));
}

template <usize op> [[gnu::noinline]] [[nodiscard]] auto io_space_create(Call& inv) noexcept -> Result {
    KernelState* const kernel = inv.cpu.runtime().kernel;
    libk_assert(kernel != nullptr);
    for (usize index = 1; index < 6; ++index)
        if (inv.trap.arg(index) != 0) return returned(MYOS_STATUS_BAD_ARGS);
    auto pool = resolve_pool(inv, cap::Right::Create);
    if (!pool) return returned(cap_status(pool.error()));
    constexpr auto charge = object::pool<io::Space>::slot_charge();
    auto txn = begin_create<io::Space>(inv, pool.value(), charge);
    if (!txn) return returned(txn.error());
    auto fee = reserve(pool.value(), charge);
    if (!fee) return returned(pool_error(fee.error()));
    auto object = txn->make(kernel->io().spaces, std::move(*fee), kernel->pmm(), kernel->io_work(),
                            kernel->io().irqs, kernel->pool<mm::Mem>(), kernel->grants());
    if (!object) return returned(MYOS_STATUS_NO_MEMORY);
    constexpr auto rights = cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect,
                                            cap::Right::Connect, cap::Right::Close, cap::Right::Revoke);
    const auto caps = std::array<cap::View, 1>{{rights}};
    if (!txn->root(kernel->grants(), caps[0])) return returned(MYOS_STATUS_NO_MEMORY);
    return publication(txn->publish(inv.cspace, caps));
}

template <usize op> [[gnu::noinline]] [[nodiscard]] auto memory_create(Call& inv) noexcept -> Result {
    KernelState* const kernel = inv.cpu.runtime().kernel;
    libk_assert(kernel != nullptr);
    arch::TrapContext& trap = inv.trap;
    auto pool = resolve_pool(inv, cap::Right::Create);
    if (!pool) {
        return returned(cap_status(pool.error()));
    }
    const usize size = trap.arg(1);
    const auto access = perms_of(trap.arg(2));
    if (size == 0 || !access) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    auto txn = begin_create<mm::Mem>(inv, pool.value(), object::pool<mm::Mem>::slot_charge());
    if (!txn) return returned(txn.error());
    auto fee = reserve(pool.value(), object::pool<mm::Mem>::slot_charge());
    if (!fee) return returned(pool_error(fee.error()));
    auto object = txn->make(kernel->pool<mm::Mem>(), std::move(*fee), kernel->pmm(), size);
    if (!object) return returned(MYOS_STATUS_NO_MEMORY);
    auto& obj = object->get();
    if (!(obj.init_anon({.access = *access}))) return returned(MYOS_STATUS_NO_MEMORY);
    const mm::MemoryTypes types = mm::MemoryTypes::of(mm::MemoryType::Normal);
    const cap::MemLimit data{mm::ObjectRange{0, obj.page_count()}, *access, types};
    const auto rights =
        cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect, cap::Right::Map,
                        cap::Right::Destroy, cap::Right::Manage, cap::Right::Revoke);
    const auto caps = std::array<cap::View, 1>{cap::View{rights, data}};
    if (!txn->root(kernel->grants(), caps[0])) return returned(MYOS_STATUS_NO_MEMORY);
    return publication(txn->publish(inv.cspace, caps));
}

template <usize op> [[gnu::noinline]] [[nodiscard]] auto memory_create_pager(Call& inv) noexcept -> Result {
    KernelState* const kernel = inv.cpu.runtime().kernel;
    libk_assert(kernel != nullptr);
    arch::TrapContext& trap = inv.trap;
    auto pool = resolve_pool(inv, cap::Right::Create);
    if (!pool) {
        return returned(cap_status(pool.error()));
    }
    auto pager = inv.cspace.resolve<Pager>(handle_of(trap.arg(3)), cap::Rights::of(cap::Right::Attach));
    const usize size = trap.arg(1);
    const auto access = perms_of(trap.arg(2));
    const usize flags = trap.arg(4);
    if (!pager) {
        return returned(cap_status(pager.error()));
    }
    auto pg_ref = pager.value().reference();
    if (!pg_ref) {
        return returned(MYOS_STATUS_BUSY);
    }
    if (size == 0 || size % mm::page_size != 0 || !access ||
        (flags & ~usize{MYOS_MEMORY_PAGER_PRIVATE}) != 0) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    auto txn = begin_create<mm::Mem>(inv, pool.value(), object::pool<mm::Mem>::slot_charge());
    if (!txn) return returned(txn.error());
    auto fee = reserve(pool.value(), object::pool<mm::Mem>::slot_charge());
    if (!fee) return returned(pool_error(fee.error()));
    auto object = txn->make(kernel->pool<mm::Mem>(), std::move(*fee), kernel->pmm(), size);
    if (!object) return returned(MYOS_STATUS_NO_MEMORY);
    auto& obj = object->get();
    if (!(obj.init_paged(std::move(*pg_ref), *access,
                               (flags & MYOS_MEMORY_PAGER_PRIVATE) != 0)))
        return returned(MYOS_STATUS_NO_MEMORY);
    const mm::MemoryTypes types = mm::MemoryTypes::of(mm::MemoryType::Normal);
    const cap::MemLimit data{mm::ObjectRange{0, obj.page_count()}, *access, types};
    const auto rights =
        cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect, cap::Right::Map,
                        cap::Right::Destroy, cap::Right::Manage, cap::Right::Revoke);
    const auto caps = std::array<cap::View, 1>{cap::View{rights, data}};
    if (!txn->root(kernel->grants(), caps[0])) return returned(MYOS_STATUS_NO_MEMORY);
    return publication(txn->publish(inv.cspace, caps));
}

template <usize op> [[gnu::noinline]] [[nodiscard]] auto vspace_create(Call& inv) noexcept -> Result {
    KernelState* const kernel = inv.cpu.runtime().kernel;
    libk_assert(kernel != nullptr);
    auto pool = resolve_pool(inv, cap::Right::Create);
    if (!pool) {
        return returned(cap_status(pool.error()));
    }
    auto txn = begin_create<mm::VSpace>(inv, pool.value(), object::pool<mm::VSpace>::slot_charge());
    if (!txn) return returned(txn.error());
    auto fee = reserve(pool.value(), object::pool<mm::VSpace>::slot_charge());
    if (!fee) return returned(pool_error(fee.error()));
    auto object = txn->make(kernel->pool<mm::VSpace>(), std::move(*fee), kernel->pmm(),
                            kernel->kernel_vspace(), kernel->space_work());
    if (!object) return returned(MYOS_STATUS_NO_MEMORY);
    auto& obj = object->get();
    if (!(obj.initialize())) return returned(MYOS_STATUS_NO_MEMORY);
    const cap::VmLimit data{
        mm::VRange{mm::Virt{mm::UserBegin}, mm::UserEnd - mm::UserBegin},
        mm::Perms::of(mm::Perm::Read, mm::Perm::Write, mm::Perm::Execute),
        mm::MemoryTypes::of(mm::MemoryType::Normal, mm::MemoryType::Uncached, mm::MemoryType::Device)};
    const auto rights =
        cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Reserve,
                        cap::Right::Delegate, cap::Right::Map, cap::Right::Unmap, cap::Right::Protect,
                        cap::Right::Inspect, cap::Right::Manage, cap::Right::Destroy, cap::Right::Revoke);
    const auto caps = std::array<cap::View, 1>{cap::View{rights, data}};
    if (!txn->root(kernel->grants(), caps[0])) return returned(MYOS_STATUS_NO_MEMORY);
    return publication(txn->publish(inv.cspace, caps));
}

template <usize op> [[gnu::noinline]] [[nodiscard]] auto cspace_create(Call& inv) noexcept -> Result {
    KernelState* const kernel = inv.cpu.runtime().kernel;
    libk_assert(kernel != nullptr);
    arch::TrapContext& trap = inv.trap;
    auto pool = resolve_pool(inv, cap::Right::Create);
    if (!pool) {
        return returned(cap_status(pool.error()));
    }
    const cap::CSpace::Quota quota{.slots = trap.arg(1), .pages = trap.arg(2)};
    if (quota.slots == 0 || quota.pages == 0) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    const auto rights = cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect,
                                        cap::Right::Manage, cap::Right::Destroy, cap::Right::Revoke);
    auto txn = begin_create<cap::CSpace>(inv, pool.value(), object::pool<cap::CSpace>::slot_charge());
    if (!txn) return returned(txn.error());
    auto fee = reserve(pool.value(), object::pool<cap::CSpace>::slot_charge());
    if (!fee) return returned(pool_error(fee.error()));
    auto object = txn->make(kernel->cspaces(), std::move(*fee), kernel->pmm(), quota);
    if (!object) return returned(MYOS_STATUS_NO_MEMORY);

    const auto caps = std::array<cap::View, 1>{cap::View{rights}};
    if (!txn->root(kernel->grants(), caps[0])) return returned(MYOS_STATUS_NO_MEMORY);
    return publication(txn->publish(inv.cspace, caps));
}

template <usize op> [[gnu::noinline]] [[nodiscard]] auto sc_create(Call& inv) noexcept -> Result {
    KernelState* const kernel = inv.cpu.runtime().kernel;
    libk_assert(kernel != nullptr);
    arch::TrapContext& trap = inv.trap;
    auto pool = resolve_pool(inv, cap::Right::Create);
    auto domain =
        inv.cspace.resolve<sched::Domain>(handle_of(trap.arg(1)), cap::Rights::of(cap::Right::Control));
    if (!pool || !domain) {
        return returned(cap_status(!pool ? pool.error() : domain.error()));
    }
    const auto budget = kernel->clock().duration_from_nanoseconds(trap.arg(2));
    const auto period = kernel->clock().duration_from_nanoseconds(trap.arg(3));
    const auto urgency = sched::Urgency::make(trap.arg(4));
    const CpuId home{trap.arg(5)};
    if (!budget || !period || !urgency) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    const sched::Sc::Config config{.budget = *budget, .period = *period, .urgency = *urgency};
    if (!sched::Sc::valid_config(config)) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    const auto rights = sc_rights();
    auto txn = begin_create<sched::Sc>(inv, pool.value(), object::pool<sched::Sc>::slot_charge());
    if (!txn) return returned(txn.error());
    auto fee = reserve(pool.value(), object::pool<sched::Sc>::slot_charge());
    if (!fee) return returned(pool_error(fee.error()));
    auto object = txn->make(kernel->sched().contexts, std::move(*fee), config, kernel->clock().now());
    if (!object) return returned(MYOS_STATUS_NO_MEMORY);
    auto& obj = object->get();

    const auto caps = std::array<cap::View, 1>{cap::View{rights}};
    if (!txn->root(kernel->grants(), caps[0])) return returned(MYOS_STATUS_NO_MEMORY);
    auto admitted = obj.admit(domain.value(), home);
    if (!admitted)
        return returned(admitted.error() == sched::Sc::Error::WrongCpu ? MYOS_STATUS_BAD_ARGS
                                                                       : MYOS_STATUS_BUSY);
    return publication(txn->publish(inv.cspace, caps));
}

struct ThreadStart final {
    arch::UserStart user{};
    myos_ipc_binding ipc{};
};

[[nodiscard]] static auto start_snapshot(Call& inv, cap::Handle handle, usize offset) noexcept
    -> std::expected<ThreadStart, myos_status_t> {
    auto snapshot = read_desc<myos_thread_start>(inv, handle, offset);
    if (!snapshot) {
        return std::unexpected(snapshot.error());
    }
    const myos_thread_start& desc = snapshot.value();
    if (desc.version != MYOS_THREAD_START_VERSION || desc.flags != 0) {
        return std::unexpected(MYOS_STATUS_BAD_ARGS);
    }
    arch::UserStart start{
        .entry = mm::Virt{desc.entry},
        .stack = mm::Virt{desc.stack},
    };
    for (usize index = 0; index < 6; ++index) {
        start.arguments[index] = desc.arguments[index];
    }
    if (!arch::valid_user_start(start)) {
        return std::unexpected(MYOS_STATUS_BAD_ARGS);
    }
    return (ThreadStart{start, desc.ipc});
}

[[gnu::noinline]] [[nodiscard]] static auto
publish_thread(Call& inv, KernelState& kernel, cap::Resolved<object::group>& pool,
               cap::Resolved<mm::VSpace>& vspace, cap::Resolved<cap::CSpace>& cspace, ThreadStart&& start,
               std::optional<ipc::Buffer>&& ipc) noexcept -> Result {
    auto stack_reservation = reserve(pool, resource::budget{.memory = mm::Stack::StackBytes});
    if (!stack_reservation) {
        return returned(pool_error(stack_reservation.error()));
    }
    auto stack_capacity = std::move(stack_reservation).value();
    auto home = mm::Stack::create(kernel.kernel_vspace());
    auto address_space = vspace.reference();
    auto capability_space = cspace.reference();
    if (!home || !address_space || !capability_space) {
        return returned(MYOS_STATUS_NO_MEMORY);
    }
    auto env =
        Env::user(std::move(address_space).value(), std::move(capability_space).value(), std::move(ipc));
    if (!env) {
        return returned(MYOS_STATUS_BUSY);
    }

    const auto total =
        add_budget(object::pool<Thread>::slot_charge(), resource::budget{.memory = mm::Stack::StackBytes});
    libk_assert(total);
    auto txn = begin_create<Thread>(inv, pool, *total);
    if (!txn) return returned(txn.error());
    auto fee = reserve(pool, object::pool<Thread>::slot_charge());
    if (!fee) return returned(pool_error(fee.error()));
    auto object = txn->make(kernel.tasks().threads, std::move(*fee), std::move(stack_capacity).commit(),
                            std::move(home).value(), std::move(env).value(), start.user);
    if (!object) return returned(MYOS_STATUS_NO_MEMORY);
    auto& obj = object->get();
    const auto rights = basic_rights();
    const auto caps = std::array<cap::View, 1>{cap::View{rights}};
    if (!txn->root(kernel.grants(), caps[0])) return returned(MYOS_STATUS_NO_MEMORY);
    if (!obj.authorize(vspace, cspace)) return returned(MYOS_STATUS_BUSY);
    return publication(txn->publish(inv.cspace, caps));
}

template <usize op> [[gnu::noinline]] [[nodiscard]] auto thread_create(Call& inv) noexcept -> Result {
    KernelState* const kernel = inv.cpu.runtime().kernel;
    libk_assert(kernel != nullptr);
    arch::TrapContext& trap = inv.trap;
    auto pool = resolve_pool(inv, cap::Right::Create);
    auto vspace = inv.cspace.resolve<mm::VSpace>(handle_of(trap.arg(1)), cap::Rights::of(cap::Right::Manage));
    auto cspace =
        inv.cspace.resolve<cap::CSpace>(handle_of(trap.arg(2)), cap::Rights::of(cap::Right::Manage));
    if (!pool || !vspace || !cspace) {
        const cap::CSpaceError error = !pool ? pool.error() : !vspace ? vspace.error() : cspace.error();
        return returned(cap_status(error));
    }
    auto start = start_snapshot(inv, handle_of(trap.arg(3)), trap.arg(4));
    if (!start || trap.arg(5) != 0) {
        return returned(start ? MYOS_STATUS_BAD_ARGS : start.error());
    }
    auto ipc = prepare_ipc(inv, *kernel, vspace.value(), start.value().ipc);
    if (!ipc) {
        return returned(ipc.error());
    }
    return publish_thread(inv, *kernel, pool.value(), vspace.value(), cspace.value(),
                          std::move(start).value(), std::move(ipc).value());
}

template <usize op> [[gnu::noinline]] [[nodiscard]] auto notification_create(Call& inv) noexcept -> Result {
    KernelState* const kernel = inv.cpu.runtime().kernel;
    libk_assert(kernel != nullptr);
    auto pool = resolve_pool(inv, cap::Right::Create);
    const u64 badge = inv.trap.arg(1);
    if (!pool) {
        return returned(cap_status(pool.error()));
    }
    if (badge == 0) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    auto txn =
        begin_create<ipc::Notification>(inv, pool.value(), object::pool<ipc::Notification>::slot_charge());
    if (!txn) return returned(txn.error());
    auto fee = reserve(pool.value(), object::pool<ipc::Notification>::slot_charge());
    if (!fee) return returned(pool_error(fee.error()));
    auto object = txn->make(kernel->ipc().notifications, std::move(*fee));
    if (!object) return returned(MYOS_STATUS_NO_MEMORY);
    const auto rights =
        cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect, cap::Right::Signal,
                        cap::Right::Receive, cap::Right::Destroy, cap::Right::Revoke);
    const cap::Badge lim{badge};
    const auto caps = std::array<cap::View, 1>{cap::View{rights, lim}};
    if (!txn->root(kernel->grants(), caps[0])) return returned(MYOS_STATUS_NO_MEMORY);
    return publication(txn->publish(inv.cspace, caps));
}

#define INST_Create(entry, nr) template auto entry<nr>(Call&) noexcept -> Result;
#define INST_Call(entry, nr)
#define INST_Ipc(entry, nr)
#define CALL(name, nr, entry, locus, unit) INST_##unit(entry, nr)
#include <uapi/calls.def>
#undef CALL
#undef INST_Call
#undef INST_Create
#undef INST_Ipc

} // namespace syscall
