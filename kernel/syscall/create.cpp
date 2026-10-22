#include <expected>
#include <bit>
#include <array>
#include <optional>
#include <syscall/call.hpp>

#include <cap/cap.hpp>
#include <object/scope.hpp>
#include <cpu/cpu.hpp>
#include <libk/checked_arithmetic.hpp>
#include <libk/inplace_vector.hpp>
#include <utility>
#include <ipc/channel.hpp>
#include <object/ref.hpp>
#include <uapi/ipc.h>
#include <ipc/notification.hpp>
#include <ipc/port.hpp>
#include <mm/table.hpp>
#include <mm/kspace.hpp>
#include <mm/mem.hpp>
#include <mm/vspace.hpp>
#include <sched/sc.hpp>
#include <task/thread.hpp>
#include <uapi/abi.h>
#include <uapi/task.h>
#include <uapi/mem.h>
#include <uapi/cap.h>
#include <mm/pager.hpp>
#include <irq/irq.hpp>
#include <io/space.hpp>

namespace syscall {

[[nodiscard]] static auto pool_quota(const cap::Resolved<resource::Quota>& pool) noexcept
    -> std::optional<cap::QuotaLimit> {
    const cap::View effective = pool.view();
    const auto* const lim = std::get_if<cap::QuotaLimit>(&effective.data);
    return lim != nullptr ? std::optional<cap::QuotaLimit>{*lim} : std::nullopt;
}

[[nodiscard]] static auto add_budget(resource::Budget first, resource::Budget second) noexcept
    -> std::optional<resource::Budget> {
    const auto memory = libk::checked_add(first.memory, second.memory);
    const auto caps = libk::checked_add(first.caps, second.caps);
    return memory && caps ? std::optional<resource::Budget>{resource::Budget{*memory, *caps}} : std::nullopt;
}

[[nodiscard]] static auto reserve(cap::Resolved<resource::Quota>& pool, resource::Budget usage) noexcept
    -> std::expected<resource::Reservation, resource::errc> {
    auto reference = pool.reference();
    if (!reference) {
        return std::unexpected(resource::errc::invalid);
    }
    return pool->reserve(std::move(reference).value(), usage);
}

[[nodiscard]] static auto pool_error(resource::errc error) noexcept -> status_t {
    switch (error) {
    case resource::errc::invalid:
        return STATUS_INVALID_CAP;
    case resource::errc::closed:
        return STATUS_BUSY;
    case resource::errc::exhausted:
        return STATUS_NO_MEMORY;
    }
    return STATUS_INTERNAL;
}

template<class T, class... Args>
static auto construct(Call& inv, resource::Reservation&& fee, Args&&... args) noexcept {
    auto& pool = inv.cpu.objects.get<T>();
    using Result = decltype(pool.create(std::move(fee), std::forward<Args>(args)...));
    using Err = typename Result::error_type;
    auto owner = inv.cspace.resolve<object::Scope>(handle_of(inv.trap.arg(6)), cap::Rights::of(cap::Right::Create));
    if (owner) {
        auto ref = owner->reference();
        auto scope = ref ? std::move(*ref).template as<object::Scope>() : std::unexpected(object::error::closed);
        if (scope) {
            auto admission = object::Scope::reserve(std::move(*scope));
            if (admission) return pool.create(std::move(*admission), std::move(fee), std::forward<Args>(args)...);
        }
    }
    if constexpr (std::is_same_v<Err, object::error>) return Result{std::unexpected(Err::closed)};
    else return Result{std::unexpected(Err::InvalidState)};
}

template <class T, usize N = 1>
static auto check_create(Call& inv, cap::Resolved<resource::Quota>& pool, resource::Budget limit) noexcept
    -> std::expected<resource::Reservation, status_t> {
    auto owner = inv.cspace.resolve<object::Scope>(handle_of(inv.trap.arg(6)), cap::Rights::of(cap::Right::Create));
    if (!owner) return std::unexpected(cap_status(owner.error()));
    const auto scope = owner->view();
    const auto* range = std::get_if<cap::ScopeLimit>(&scope.data);
    if (!range || !(range->kinds & OBJ_BIT(static_cast<u16>(object::kind<T>))))
        return std::unexpected(STATUS_DENIED);
    const auto quota = pool_quota(pool);
    std::optional<resource::Budget> publication = cap::Graph::node_usage();
    for (usize i = 0; N > 1 && i < N && publication; ++i)
        publication = add_budget(*publication, inv.cpu.grants.node_usage());
    if (!quota || !(quota->object_kinds & OBJ_BIT(static_cast<u16>(object::kind<T>))) || !quota->budget.contains(limit) ||
        !publication || !quota->budget.contains(*publication))
        return std::unexpected(STATUS_DENIED);
    auto admission = reserve(pool, cap::Graph::node_usage());
    if (!admission) return std::unexpected(pool_error(admission.error()));
    return std::move(*admission);
}

template <usize N>
static auto publication(std::expected<std::array<cap::Handle, N>, cap::CSpace::PublishError> handles) noexcept
    -> Result {
    if (!handles)
        return returned(std::visit(
            [](auto error) -> status_t {
                if constexpr (std::is_same_v<decltype(error), resource::errc>)
                    return pool_error(error);
                else if constexpr (std::is_same_v<decltype(error), cap::CSpaceError>)
                    return cap_status(error);
                else
                    return STATUS_NO_MEMORY;
            },
            handles.error()));
    return Result{STATUS_OK, (*handles)[0].raw(), Disposition::Return,
                  N > 1 ? (*handles)[N - 1].raw() : 0};
}

template<class Pending, usize N, class Bind>
static auto publish(Call& inv, Pending& object, cap::GrantRef& root,
                    cap::Resolved<resource::Quota>& pool, const std::array<cap::View, N>& views,
                    Bind&& bind) noexcept {
    using Result = std::expected<std::array<cap::Handle, N>, cap::CSpace::PublishError>;
    if constexpr (N == 1) {
        auto handles = inv.cspace.publish(views, [&](cap::View) -> std::expected<cap::GrantRef, cap::CSpace::PublishError> {
            return std::move(root);
        }, std::forward<Bind>(bind));
        if (handles) object.publish().reset();
        return handles;
    }
    auto pin = root.acquire();
    if (!pin) return Result{std::unexpected(pin.error())};
    auto handles = inv.cspace.publish(views, [&](cap::View view)
        -> std::expected<cap::GrantRef, cap::CSpace::PublishError> {
        auto fee = reserve(pool, cap::Graph::node_usage());
        if (!fee) return std::unexpected(fee.error());
        auto target = pin->clone_target();
        if (!target) return std::unexpected(cap::GrantError::InvalidState);
        auto grant = inv.cpu.grants.derive(std::move(*fee), *pin, std::move(*target), view);
        if (!grant) return std::unexpected(grant.error());
        return std::move(*grant);
    }, std::forward<Bind>(bind));
    if (handles) object.publish().reset();
    return handles;
}

template<class Pending, usize N>
static auto publish(Call& inv, Pending& object, cap::GrantRef& root,
                    cap::Resolved<resource::Quota>& pool, const std::array<cap::View, N>& views) noexcept {
    return publish(inv, object, root, pool, views, [](auto&, usize) { return true; });
}

// A created object's persistent dependencies and its result enter the same
// Grant/Gate boundary. Hardware activation follows while the creator pins it.
template<class Pending, usize N>
static auto publish_bound(Call& inv, Pending& object, cap::GrantRef& root, cap::View view,
                          std::array<const cap::GrantPin*, N> grants) noexcept -> Result {
    auto slot = inv.cspace.reserve();
    if (!slot) return returned(cap_status(slot.error()));
    auto prepared = inv.cspace.prepare(std::move(*slot), std::move(root), view);
    if (!prepared) return returned(cap_status(prepared.error()));
    const auto handle = prepared->handle();
    object::ref<> self = object.reference();
    const object::ref<>* gates[]{&self};
    const auto status = object.get().admit([&](auto cells, auto install) noexcept {
        std::array<cap::Graph::Attach, N> relations;
        for (usize i = 0; i < N; ++i) relations[i] = {grants[i], cells[i]};
        return inv.cspace.insert(std::span{&*prepared, 1}, inv.cpu.grants.admission(grants), gates,
            install, relations) ? STATUS_OK : STATUS_CLOSED;
    });
    if (status != STATUS_OK) return returned(status);
    return returned(STATUS_OK, handle.raw());
}

[[nodiscard]] static auto resolve_pool(Call& inv, cap::Right right) noexcept
    -> std::expected<cap::Resolved<resource::Quota>, cap::CSpaceError> {
    return inv.cspace.resolve<resource::Quota>(handle_of(inv.trap.arg(0)), cap::Rights::of(right));
}

[[nodiscard]] static constexpr auto basic_rights() noexcept -> cap::Rights {
    return cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect,
                           cap::Right::Control, cap::Right::Observe, cap::Right::Destroy, cap::Right::Adopt, cap::Right::Reassign, cap::Right::Revoke);
}

[[nodiscard]] static constexpr auto sc_rights() noexcept -> cap::Rights {
    return cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect,
                           cap::Right::Control, cap::Right::Attach, cap::Right::Destroy, cap::Right::Adopt, cap::Right::Reassign, cap::Right::Revoke);
}

[[nodiscard]] static auto mem_permits(const cap::Resolved<mm::Mem>& memory, mm::ObjectRange range,
                                        mm::Perms access) noexcept -> bool {
    const cap::View effective = memory.view();
    const auto* const lim = std::get_if<cap::MemLimit>(&effective.data);
    return lim != nullptr && lim->range.contains(range) && lim->perms.contains(access);
}

[[nodiscard]] static auto bind_ipc(cap::Resolved<mm::VSpace>& vspace,
                                   cap::Resolved<mm::Mem>& memory,
                                   const IpcBinding& desc) noexcept
    -> std::expected<mm::View, status_t> {
    if (desc.pages == 0 || desc.pages > IPC_BUFFER_MAX_PAGES) {
        return std::unexpected(STATUS_BAD_ARGS);
    }
    const auto bytes = libk::checked_multiply(desc.pages, mm::page_size);
    const mm::va_t address{desc.address};
    if (!bytes || !address.is_aligned(mm::page_size)) {
        return std::unexpected(STATUS_BAD_ARGS);
    }
    const auto rw = mm::Perms::of(mm::Perm::Read, mm::Perm::Write);
    const mm::ObjectRange object{desc.page, desc.pages};
    if (!mem_permits(memory, object, rw)) {
        return std::unexpected(STATUS_DENIED);
    }
    auto reference = memory.reference();
    if (!reference) {
        return std::unexpected(STATUS_BUSY);
    }
    auto buffer = vspace->bind_view({.memory = std::move(*reference), .object = object,
        .virtual_range = mm::VRange{address, *bytes}, .perms = rw});
    return buffer ? std::expected<mm::View, status_t>{(std::move(buffer).value())}
                  : std::unexpected(mm::status(buffer.error()));
}

[[nodiscard]] static auto prepare_ipc(Call& inv, cap::Resolved<mm::VSpace>& vspace,
                                      const IpcBinding& desc) noexcept
    -> std::expected<std::optional<mm::View>, status_t> {
    if (desc.pages == 0) {
        if (desc.memory != 0 || desc.page != 0 || desc.address != 0) {
            return std::unexpected(STATUS_BAD_ARGS);
        }
        return (std::optional<mm::View>{std::nullopt});
    }
    auto memory = inv.cspace.resolve<mm::Mem>(handle_of(desc.memory), cap::Rights::of(cap::Right::Map));
    if (!memory) {
        return std::unexpected(cap_status(memory.error()));
    }
    auto buffer = bind_ipc(vspace, memory.value(), desc);
    if (!buffer) {
        return std::unexpected(buffer.error());
    }
    return (std::optional<mm::View>{std::move(buffer).value()});
}

template<class T, class Provider, class Run>
[[gnu::noinline]] static auto submit_op(Call& inv, cap::Resolved<Provider>& target,
    std::span<const cap::GrantPin*> extra, Run&& run) noexcept -> Result {
    auto quota = resolve_pool(inv, cap::Right::Create);
    auto owner = inv.cspace.resolve<object::Scope>(handle_of(inv.trap.arg(6)), cap::Rights::of(cap::Right::Create));
    if (!quota || !owner) return returned(cap_status(!quota ? quota.error() : owner.error()));
    auto admission = check_create<T>(inv, *quota, object::pool<T>::slot_usage());
    if (!admission) return returned(admission.error());
    auto fee = reserve(*quota, object::pool<T>::slot_usage());
    if (!fee) return returned(pool_error(fee.error()));
    auto provider = target.reference(), quota_ref = quota->reference(), scope = owner->reference();
    if (!provider || !quota_ref || !scope) return returned(STATUS_CLOSED);
    auto object = construct<T>(inv, std::move(*fee), *provider);
    if (!object) return returned(STATUS_NO_MEMORY);
    auto cpu = inv.target->cpu_domain();
    if (!cpu) return returned(STATUS_DENIED);
    object->get().op().cpu_domain(std::move(cpu));
    const cap::View result_view{cap::Rights::from_raw(basic_rights().raw() | RIGHT_OBSERVE), std::monostate{}};
    auto root = object->root(inv.cpu.grants, std::move(*admission), result_view);
    if (!root) return returned(STATUS_NO_MEMORY);
    auto slot = inv.cspace.reserve();
    if (!slot) return returned(cap_status(slot.error()));
    auto prepared = inv.cspace.prepare(std::move(*slot), std::move(*root), result_view);
    if (!prepared) return returned(cap_status(prepared.error()));
    const auto handle = prepared->handle();
    object::ref<> execution = object->reference();
    std::array<const cap::GrantPin*, IPC_MAX_CAPS> grants{&target.pin(), &quota->pin(), &owner->pin()};
    libk_assert(extra.size() <= grants.size() - 3);
    std::copy(extra.begin(), extra.end(), grants.begin() + 3);
    std::array<const object::ref<>*, 3> gates{&*provider, &*quota_ref, &*scope};
    auto publish = [&](Source::Action install, std::span<const cap::GrantPin*> dependencies) noexcept {
        const auto count = 3 + extra.size();
        libk_assert(dependencies.size() <= grants.size() - count);
        std::copy(dependencies.begin(), dependencies.end(), grants.begin() + count);
        return object->get().op().accept(install, [&](std::span<cap::Attachment*> cells, const object::ref<>& cpu, Source::Action commit) noexcept {
            const cap::Graph::Attach relations[]{{&target.pin(), cells[0]}, {nullptr, cells[1], &cpu}};
            return inv.cspace.insert(std::span{&*prepared, 1},
                inv.cpu.grants.admission(std::span{grants}.first(count + dependencies.size())), gates, commit,
                relations) ? STATUS_OK : STATUS_CLOSED;
        });
    };
    const auto status = run(Op::Submission{&execution, decltype(Op::Submission::publish)::bind(publish)});
    if (execution) return returned(status == STATUS_OK ? STATUS_INTERNAL : status);
    object->get().op().publish();
    object->publish().reset();
    return returned(STATUS_PENDING, handle.raw());
}

template<usize op> [[gnu::noinline]] auto vm_submit(Call& inv) noexcept -> Result {
    const auto kind = inv.trap.arg(2);
    if (kind < VM_OP_MAP || kind > VM_OP_CLEAR || inv.trap.arg(4) || inv.trap.arg(5)) return returned(STATUS_BAD_ARGS);
    VmReq request{};
    auto* buffer = inv.target->ipc_buffer();
    if (!buffer || !buffer->read(inv.trap.arg(3), std::span<byte>{reinterpret_cast<byte*>(&request), sizeof(request)}))
        return returned(STATUS_BAD_ARGS);
    const auto required = kind == VM_OP_MAP ? cap::Right::Map : kind == VM_OP_UNMAP ? cap::Right::Unmap
        : kind == VM_OP_PROTECT ? cap::Right::Protect : cap::Right::Destroy;
    auto target = inv.cspace.resolve<mm::VSpace>(handle_of(inv.trap.arg(1)), cap::Rights::of(required));
    if (!target) return returned(cap_status(target.error()));
    const auto view = target->view();
    const auto* where = std::get_if<cap::VmLimit>(&view.data);
    libk_assert(where);
    const auto range = kind == VM_OP_CLEAR ? std::optional{where->range} : range_of(request.address, request.bytes);
    const auto perms = perms_of(request.perms & (VM_READ | VM_WRITE | VM_EXECUTE));
    if (!range || ((kind == VM_OP_MAP || kind == VM_OP_PROTECT) && !perms)
        || (request.perms & ~(VM_READ | VM_WRITE | VM_EXECUTE | (kind == VM_OP_MAP ? VM_MAP_PRIVATE : 0)))
        || (kind != VM_OP_MAP && (request.memory || request.page))
        || (kind == VM_OP_CLEAR && (request.address || request.bytes || request.perms))) return returned(STATUS_BAD_ARGS);
    std::optional<cap::Resolved<mm::Mem>> memory;
    if (kind == VM_OP_MAP) {
        auto resolved = inv.cspace.resolve<mm::Mem>(handle_of(request.memory), cap::Rights::of(cap::Right::Map));
        if (!resolved) return returned(cap_status(resolved.error()));
        memory.emplace(std::move(*resolved));
    }
    std::array<const cap::GrantPin*, 1> extra{memory ? &memory->pin() : &target->pin()};
    return submit_op<mm::VmOp>(inv, *target, extra, [&](Op::Submission submission) noexcept -> status_t {
        const mm::VmCtx ctx{inv.cpu.cpus, inv.cpu.id, submission};
        auto& space = target->object();
        std::expected<mm::VmStatus, mm::VSpaceError> result;
        if (kind == VM_OP_MAP) {
            auto mapped = space.map(ctx, *where, {*range, {request.page, range->size() / mm::page_size}, *perms,
                bool(request.perms & VM_MAP_PRIVATE)}, *memory);
            result = mapped ? std::expected<mm::VmStatus, mm::VSpaceError>{mapped->status} : std::unexpected(mapped.error());
        } else if (kind == VM_OP_UNMAP) result = space.unmap(ctx, *where, *range);
        else if (kind == VM_OP_PROTECT) result = space.protect(ctx, *where, *range, *perms);
        else result = space.clear(ctx, *range);
        return result ? STATUS_OK : mm::status(result.error());
    });
}

template<usize op> [[gnu::noinline]] auto io_submit(Call& inv) noexcept -> Result {
    const auto kind = inv.trap.arg(2);
    if ((kind != IO_OP_MAP && kind != IO_OP_UNMAP) || inv.trap.arg(4) || inv.trap.arg(5)) return returned(STATUS_BAD_ARGS);
    IoReq request{};
    auto* buffer = inv.target->ipc_buffer();
    if (!buffer || !buffer->read(inv.trap.arg(3), std::span<byte>{reinterpret_cast<byte*>(&request), sizeof(request)}))
        return returned(STATUS_BAD_ARGS);
    auto target = inv.cspace.resolve<io::Space>(handle_of(inv.trap.arg(1)), cap::Rights::of(
        kind == IO_OP_MAP ? cap::Right::Map : cap::Right::Unmap));
    if (!target) return returned(cap_status(target.error()));
    const auto perms = perms_of(request.perms);
    if (!request.count || request.address % mm::page_size || (kind == IO_OP_MAP && !perms)
        || (kind == IO_OP_UNMAP && (request.memory || request.page || request.perms))) return returned(STATUS_BAD_ARGS);
    std::optional<cap::Resolved<mm::Mem>> memory;
    if (kind == IO_OP_MAP) {
        auto resolved = inv.cspace.resolve<mm::Mem>(handle_of(request.memory), cap::Rights::of(cap::Right::Map));
        if (!resolved) return returned(cap_status(resolved.error()));
        memory.emplace(std::move(*resolved));
    }
    std::array<const cap::GrantPin*, 1> extra{memory ? &memory->pin() : &target->pin()};
    return submit_op<io::IoOp>(inv, *target, extra, [&](Op::Submission submission) noexcept -> status_t {
        if (kind == IO_OP_UNMAP) return target->object().unmap(submission, request.address, request.count);
        auto mapped = target->object().map(submission, *memory, {request.page, request.count}, request.address, *perms);
        return mapped ? STATUS_OK : mapped.error();
    });
}

template <usize op> [[nodiscard]] auto channel_create(Call& inv) noexcept -> Result {
    auto pool = resolve_pool(inv, cap::Right::Create);
    if (!pool) {
        return returned(cap_status(pool.error()));
    }
    const auto config = ipc::ChannelConfig{
        .queue_capacity = inv.trap.arg(1),
        .max_words = inv.trap.arg(2),
        .max_caps = inv.trap.arg(3),
    };
    if (config.queue_capacity == 0 || config.queue_capacity > CHANNEL_MAX_QUEUE ||
        config.max_words == 0 || config.max_words > CHANNEL_MAX_WORDS ||
        config.max_caps > CHANNEL_MAX_CAPS) {
        return returned(STATUS_BAD_ARGS);
    }
    const auto bytes = ipc::Channel::storage_bytes(config);
    const auto cost =
        bytes ? add_budget(object::pool<ipc::Channel>::slot_usage(), resource::Budget{.memory = *bytes})
              : std::nullopt;
    if (!cost) return returned(STATUS_BAD_ARGS);
    auto admission = check_create<ipc::Channel, 2>(inv, pool.value(), *cost);
    if (!admission) return returned(admission.error());
    auto fee = reserve(pool.value(), object::pool<ipc::Channel>::slot_usage());
    if (!fee) return returned(pool_error(fee.error()));
    auto object = construct<ipc::Channel>(inv, std::move(*fee), inv.cpu.objects.pmm(), config);
    if (!object) return returned(STATUS_NO_MEMORY);
    auto& obj = object->get();
    if (!obj.open()) return returned(STATUS_NO_MEMORY);
    const auto rights =
        cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect, cap::Right::Send,
                        cap::Right::Receive, cap::Right::Close, cap::Right::Destroy, cap::Right::Adopt, cap::Right::Reassign, cap::Right::Revoke);
    const auto view = [&](cap::ChannelSide side) {
        return cap::View{rights, cap::ChanLimit{.side = side, .badge = 0, .fixed = 0}};
    };
    const auto [ceiling, caps] = std::pair{view(cap::ChannelSide::Any),
                                           std::array{view(cap::ChannelSide::A), view(cap::ChannelSide::B)}};
    auto root = object->root(inv.cpu.grants, std::move(*admission), ceiling);
    if (!root) return returned(STATUS_NO_MEMORY);
    auto handles = publish(inv, *object, *root, pool.value(), caps, [&](cap::GrantRef& grant, usize i) {
        return bool(obj.bind_side_root(grant, i == 0 ? cap::ChannelSide::A : cap::ChannelSide::B));
    });
    return publication(std::move(handles));
}

template <usize op> [[gnu::noinline]] [[nodiscard]] auto pager_create(Call& inv) noexcept -> Result {
    auto pool = resolve_pool(inv, cap::Right::Create);
    if (!pool) return returned(cap_status(pool.error()));
    auto admission = check_create<Pager>(inv, pool.value(), object::pool<Pager>::slot_usage());
    if (!admission) return returned(admission.error());
    auto fee = reserve(pool.value(), object::pool<Pager>::slot_usage());
    if (!fee) return returned(pool_error(fee.error()));
    auto object = construct<Pager>(inv, std::move(*fee));
    if (!object) return returned(STATUS_NO_MEMORY);
    const auto rights =
        cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect, cap::Right::Attach,
                        cap::Right::Serve, cap::Right::Supply, cap::Right::Fail, cap::Right::WritebackAck,
                        cap::Right::Close, cap::Right::Destroy, cap::Right::Adopt, cap::Right::Reassign, cap::Right::Revoke);
    const auto caps = std::array<cap::View, 1>{cap::View{rights}};
    auto root = object->root(inv.cpu.grants, std::move(*admission), caps[0]);
    if (!root) return returned(STATUS_NO_MEMORY);
    return publication(publish(inv, *object, *root, pool.value(), caps));
}

template <usize op> [[gnu::noinline]] [[nodiscard]] auto quota_create(Call& inv) noexcept -> Result {
    arch::TrapCtx& trap = inv.trap;
    auto pool = resolve_pool(inv, cap::Right::Split);
    if (!pool) {
        return returned(cap_status(pool.error()));
    }
    const resource::Budget limit{.memory = trap.arg(1), .caps = trap.arg(2)};
    const resource::Budget ceiling = trap.arg(4) || trap.arg(5)
        ? resource::Budget{trap.arg(4), trap.arg(5)} : limit;
    const u64 kinds = trap.arg(3);
    const auto parent = pool_quota(pool.value());
    if (!parent || !ceiling.contains(limit) || !parent->budget.contains(ceiling) || (kinds & ~parent->object_kinds) != 0 || kinds == 0) {
        return returned(STATUS_DENIED);
    }
    const auto rights =
        cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect, cap::Right::Create,
                        cap::Right::Control, cap::Right::Split, cap::Right::Close, cap::Right::Destroy, cap::Right::Adopt, cap::Right::Reassign, cap::Right::Revoke);
    auto admission = check_create<resource::Quota>(inv, pool.value(), limit);
    if (!admission) return returned(admission.error());
    auto fee = reserve(pool.value(), object::pool<resource::Quota>::slot_usage());
    if (!fee) return returned(pool_error(fee.error()));
    resource::Usage delegation;
    if (!limit.empty()) {
        auto reserved = reserve(pool.value(), limit);
        if (!reserved) return returned(pool_error(reserved.error()));
        delegation = std::move(*reserved).commit();
    }
    auto object = construct<resource::Quota>(inv, std::move(*fee), limit, std::move(delegation));
    if (!object) return returned(STATUS_NO_MEMORY);
    const cap::QuotaLimit data{ceiling, kinds};
    const auto caps = std::array<cap::View, 1>{cap::View{rights, data}};
    auto root = object->root(inv.cpu.grants, std::move(*admission), caps[0]);
    if (!root) return returned(STATUS_NO_MEMORY);
    return publication(publish(inv, *object, *root, pool.value(), caps));
}

template<usize op> [[nodiscard]] auto scope_create(Call& inv) noexcept -> Result {
    auto quota = resolve_pool(inv, cap::Right::Create);
    if (!quota) return returned(cap_status(quota.error()));
    auto admission = check_create<object::Scope>(inv, *quota, object::pool<object::Scope>::slot_usage());
    if (!admission) return returned(admission.error());
    auto fee = reserve(*quota, object::pool<object::Scope>::slot_usage());
    if (!fee) return returned(pool_error(fee.error()));
    auto scope = construct<object::Scope>(inv, std::move(*fee));
    if (!scope) return returned(STATUS_CLOSED);
    const auto rights = cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect,
        cap::Right::Create, cap::Right::Manage, cap::Right::Close, cap::Right::Destroy, cap::Right::Adopt, cap::Right::Reassign, cap::Right::Revoke);
    auto parent = inv.cspace.resolve<object::Scope>(handle_of(inv.trap.arg(6)), cap::Rights::of(cap::Right::Create));
    if (!parent) return returned(cap_status(parent.error()));
    const auto parent_view = parent->view();
    const auto* limit = std::get_if<cap::ScopeLimit>(&parent_view.data);
    libk_assert(limit);
    const auto caps = std::array{cap::View{rights, cap::ScopeLimit{.kinds=limit->kinds}}};
    auto root = scope->root(inv.cpu.grants, std::move(*admission), caps[0]);
    if (!root) return returned(STATUS_NO_MEMORY);
    return publication(publish(inv, *scope, *root, *quota, caps));
}

static auto publish_memory(Call& inv, cap::Resolved<resource::Quota>& pool, usize size,
                           mm::Perms access, mm::Mem::Config config, const cap::GrantPin* dependency = nullptr) noexcept -> Result {
    auto admission = check_create<mm::Mem>(inv, pool, object::pool<mm::Mem>::slot_usage());
    if (!admission) return returned(admission.error());
    auto fee = reserve(pool, object::pool<mm::Mem>::slot_usage());
    if (!fee) return returned(pool_error(fee.error()));
    auto object = construct<mm::Mem>(inv, std::move(*fee), inv.cpu.objects.pmm(), size,
                                                       std::move(config));
    if (!object) return returned(mem_status(object.error()));
    const cap::MemLimit data{{0, object->get().page_count()}, access};
    const auto rights = cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate,
        cap::Right::Inspect, cap::Right::Map, cap::Right::Destroy, cap::Right::Manage, cap::Right::Adopt, cap::Right::Reassign, cap::Right::Revoke);
    const auto caps = std::array<cap::View, 1>{cap::View{rights, data}};
    auto root = object->root(inv.cpu.grants, std::move(*admission), caps[0]);
    if (!root) return returned(STATUS_NO_MEMORY);
    if (dependency) {
        auto result = publish_bound(inv, *object, *root, caps[0], std::array{dependency});
        if (result.status == STATUS_OK) object->publish().reset();
        return result;
    }
    return publication(publish(inv, *object, *root, pool, caps));
}
template <usize op> [[gnu::noinline]] [[nodiscard]] auto memory_create(Call& inv) noexcept -> Result {
    auto pool = resolve_pool(inv, cap::Right::Create);
    if (!pool) return returned(cap_status(pool.error()));
    const auto size = inv.trap.arg(1);
    const auto access = perms_of(inv.trap.arg(2));
    if (!size || size % mm::page_size || !access) return returned(STATUS_BAD_ARGS);
    return publish_memory(inv, *pool, size, *access, mm::AnonCfg{.perms = *access});
}
template <usize op> [[gnu::noinline]] [[nodiscard]] auto memory_create_pager(Call& inv) noexcept -> Result {
    auto pool = resolve_pool(inv, cap::Right::Create);
    auto pager = inv.cspace.resolve<Pager>(handle_of(inv.trap.arg(3)), cap::Rights::of(cap::Right::Attach));
    if (!pool || !pager) return returned(cap_status(!pool ? pool.error() : pager.error()));
    const auto size = inv.trap.arg(1), flags = inv.trap.arg(4), credits = inv.trap.arg(5);
    const auto access = perms_of(inv.trap.arg(2));
    if (!size || size % mm::page_size || !access || !credits || (flags & ~usize{MEMORY_PAGER_PRIVATE}))
        return returned(STATUS_BAD_ARGS);
    auto reference = pager->reference();
    if (!reference) return returned(STATUS_BUSY);
    return publish_memory(inv, *pool, size, *access,
                          mm::PagedCfg{std::move(*reference), *access, bool(flags & MEMORY_PAGER_PRIVATE), credits}, &pager->pin());
}
static auto physical_claim(cap::Resolved<mm::Phys>& physical, cap::Resolved<resource::Quota>& pool,
                           usize address, usize bytes) noexcept -> std::expected<mm::Phys::Claim, status_t> {
    const auto pages = mm::Pages::from_aligned_bytes(mm::pa_t{address}, bytes);
    const auto view = physical.view();
    const auto* limit = std::get_if<cap::PhysLimit>(&view.data);
    if (!pages || !limit || !limit->range.contains(*pages)) return std::unexpected(STATUS_DENIED);
    auto owner = physical.reference();
    auto quota = pool.reference();
    if (!owner || !quota) return std::unexpected(STATUS_BUSY);
    auto typed = std::move(*owner).as<mm::Phys>();
    if (!typed) return std::unexpected(STATUS_INVALID_CAP);
    return physical.object().claim(std::move(*typed), *quota, *pages);
}
template <usize op> [[gnu::noinline]] [[nodiscard]] auto memory_create_physical(Call& inv) noexcept -> Result {
    auto pool = resolve_pool(inv, cap::Right::Create);
    if (!pool) return returned(cap_status(pool.error()));
    const auto address = inv.trap.arg(2), size = inv.trap.arg(3);
    const auto access = perms_of(inv.trap.arg(4));
    if (!size || !access || access->contains(mm::Perm::Execute) || inv.trap.arg(5))
        return returned(STATUS_BAD_ARGS);
    auto physical = inv.cspace.resolve<mm::Phys>(handle_of(inv.trap.arg(1)), cap::Rights::of(cap::Right::Create));
    if (!physical) return returned(cap_status(physical.error()));
    auto claim = physical_claim(*physical, *pool, address, size);
    if (!claim) return returned(claim.error());
    const auto pages = *mm::Pages::from_aligned_bytes(mm::pa_t{address}, size);
    const mm::Extent extent{{0, pages.page_count()}, pages, *access};
    return publish_memory(inv, *pool, size, *access, mm::PhysCfg{{&extent, 1}, {}, std::move(*claim)}, &physical->pin());
}
template <usize op> [[gnu::noinline]] [[nodiscard]] auto irq_controller_create(Call& inv) noexcept -> Result {
    auto pool = resolve_pool(inv, cap::Right::Create);
    if (!pool) return returned(cap_status(pool.error()));
    const auto config = read_desc<IrqConfig>(inv, handle_of(inv.trap.arg(2)), inv.trap.arg(3));
    if (!config) return returned(config.error());
    if (config->protocol != IRQ_PLIC || !config->sources || config->sources >= 1024 || config->reserved ||
        !inv.cpu.cpus->get(CpuId{config->cpu}) || config->context > (SIZE_MAX - 0x200008) / 0x1000 ||
        config->bytes < 0x200008 + config->context * 0x1000 || inv.trap.arg(4) || inv.trap.arg(5))
        return returned(STATUS_BAD_ARGS);
    constexpr auto usage = object::pool<irq::Ctl>::slot_usage();
    auto admission = check_create<irq::Ctl>(inv, *pool, usage);
    if (!admission) return returned(admission.error());
    auto physical = inv.cspace.resolve<mm::Phys>(handle_of(inv.trap.arg(1)), cap::Rights::of(cap::Right::Control));
    if (!physical) return returned(cap_status(physical.error()));
    auto claim = physical_claim(*physical, *pool, config->address, config->bytes);
    if (!claim) return returned(claim.error());
    auto fee = reserve(*pool, usage);
    if (!fee) return returned(pool_error(fee.error()));
    auto object = construct<irq::Ctl>(inv, std::move(*fee), inv.kspace, *inv.cpu.cpus,
                                inv.cpu.objects.work(), std::move(*claim), *config);
    if (!object) return returned(STATUS_NO_MEMORY);
    auto opening = object->reference().admit();
    if (!opening) return returned(STATUS_CLOSED);
    const auto status = object->get().open();
    if (status != STATUS_OK) return returned(status);
    const auto rights = cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect,
                                       cap::Right::Create, cap::Right::Destroy, cap::Right::Adopt, cap::Right::Reassign, cap::Right::Revoke);
    const auto caps = std::array<cap::View, 1>{cap::View{rights, cap::IndexLimit{1, config->sources}}};
    auto root = object->root(inv.cpu.grants, std::move(*admission), caps[0]);
    if (!root) return returned(STATUS_NO_MEMORY);
    auto result = publish_bound(inv, *object, *root, caps[0], std::array{&physical->pin()});
    if (result.status == STATUS_OK) { object->get().activate(); object->publish().reset(); }
    return result;
}
template <usize op> [[gnu::noinline]] [[nodiscard]] auto irq_create(Call& inv) noexcept -> Result {
    auto pool = resolve_pool(inv, cap::Right::Create);
    auto controller = inv.cspace.resolve<irq::Ctl>(handle_of(inv.trap.arg(1)), cap::Rights::of(cap::Right::Create));
    if (!pool || !controller) return returned(cap_status(!pool ? pool.error() : controller.error()));
    const auto view = controller->view();
    const auto* limit = std::get_if<cap::IndexLimit>(&view.data);
    const auto id = inv.trap.arg(2);
    if (!limit || id < limit->first || id - limit->first >= limit->count || id > UINT32_MAX)
        return returned(STATUS_DENIED);
    auto line = controller->object().line(id);
    if (!line) return returned(line.error());
    auto admission = check_create<irq::Irq>(inv, *pool, object::pool<irq::Irq>::slot_usage());
    if (!admission) return returned(admission.error());
    auto fee = reserve(*pool, object::pool<irq::Irq>::slot_usage());
    auto owner = controller->reference();
    if (!fee || !owner) return returned(STATUS_NO_MEMORY);
    auto typed = std::move(*owner).as<irq::Ctl>();
    if (!typed) return returned(STATUS_INVALID_CAP);
    auto object = construct<irq::Irq>(inv, std::move(*fee), *line, std::move(*typed));
    if (!object) return returned(STATUS_NO_MEMORY);
    const auto rights = cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect,
        cap::Right::Route, cap::Right::Observe, cap::Right::Ack, cap::Right::Destroy, cap::Right::Adopt, cap::Right::Reassign, cap::Right::Revoke);
    const auto caps = std::array<cap::View, 1>{cap::View{rights, cap::IrqRoute{u32(id), true}}};
    auto root = object->root(inv.cpu.grants, std::move(*admission), caps[0]);
    if (!root) return returned(STATUS_NO_MEMORY);
    auto result = publish_bound(inv, *object, *root, caps[0], std::array{&controller->pin()});
    if (result.status == STATUS_OK) object->publish().reset();
    return result;
}

template <usize op> [[gnu::noinline]] [[nodiscard]] auto iommu_create(Call& inv) noexcept -> Result {
    auto pool = resolve_pool(inv, cap::Right::Create);
    if (!pool) return returned(cap_status(pool.error()));
    const auto cfg = read_desc<IommuConfig>(inv, handle_of(inv.trap.arg(2)), inv.trap.arg(3));
    if (!cfg) return returned(cfg.error());
    if (cfg->protocol != IOMMU_RISCV || cfg->reserved || cfg->device_bits < 1 || cfg->device_bits > 24 ||
        cfg->bytes < 4096 || inv.trap.arg(4) || inv.trap.arg(5)) return returned(STATUS_BAD_ARGS);
    auto irq = inv.cspace.resolve<irq::Ctl>(handle_of(cfg->irq_controller), cap::Rights::of(cap::Right::Create));
    if (!irq) return returned(cap_status(irq.error()));
    const auto view = irq->view();
    const auto* limit = std::get_if<cap::IndexLimit>(&view.data);
    if (!limit || cfg->source < limit->first || cfg->source - limit->first >= limit->count) return returned(STATUS_DENIED);
    auto line = irq->object().line(cfg->source);
    if (!line) return returned(line.error());
    constexpr auto usage = object::pool<io::Iommu>::slot_usage();
    auto admission = check_create<io::Iommu>(inv, *pool, usage);
    if (!admission) return returned(admission.error());
    auto physical = inv.cspace.resolve<mm::Phys>(handle_of(inv.trap.arg(1)), cap::Rights::of(cap::Right::Control));
    if (!physical) return returned(cap_status(physical.error()));
    auto claim = physical_claim(*physical, *pool, cfg->address, cfg->bytes);
    if (!claim) return returned(claim.error());
    auto fee = reserve(*pool, usage);
    auto quota = pool->reference();
    auto owner = irq->reference();
    if (!fee || !quota || !owner) return returned(STATUS_NO_MEMORY);
    auto typed = std::move(*owner).as<irq::Ctl>();
    if (!typed) return returned(STATUS_INVALID_CAP);
    auto object = construct<io::Iommu>(inv, std::move(*fee), inv.cpu.objects.pmm(), inv.kspace,
        *inv.cpu.cpus, inv.cpu.objects.work(), inv.cpu.dispatcher().clock(), std::move(*quota),
        std::move(*claim), std::move(*typed), *line, *cfg);
    if (!object) return returned(STATUS_NO_MEMORY);
    auto opening = object->reference().admit();
    if (!opening) return returned(STATUS_CLOSED);
    const auto status = object->get().open(inv.target->cpu_domain());
    if (status != STATUS_OK) return returned(status);
    const auto rights = cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect,
                                       cap::Right::Create, cap::Right::Destroy, cap::Right::Adopt, cap::Right::Reassign, cap::Right::Revoke);
    const auto caps = std::array<cap::View, 1>{cap::View{rights, cap::IndexLimit{0, usize{1} << cfg->device_bits}}};
    auto root = object->root(inv.cpu.grants, std::move(*admission), caps[0]);
    if (!root) return returned(STATUS_NO_MEMORY);
    auto result = publish_bound(inv, *object, *root, caps[0], std::array{&physical->pin(), &irq->pin()});
    if (result.status == STATUS_OK) {
        const auto activated = object->get().activate();
        if (activated != STATUS_OK) {
            static_cast<void>(inv.cspace.close(handle_of(result.value)));
            static_cast<void>(object->reference().destroy());
            return returned(activated);
        }
        object->publish().reset();
    }
    return result;
}
template <usize op> [[gnu::noinline]] [[nodiscard]] auto io_space_create(Call& inv) noexcept -> Result {
    auto pool = resolve_pool(inv, cap::Right::Create);
    auto ctl = inv.cspace.resolve<io::Iommu>(handle_of(inv.trap.arg(1)), cap::Rights::of(cap::Right::Create));
    if (!pool || !ctl) return returned(cap_status(!pool ? pool.error() : ctl.error()));
    const auto view = ctl->view();
    const auto* limit = std::get_if<cap::IndexLimit>(&view.data);
    const usize requester = inv.trap.arg(2);
    if (!limit || requester < limit->first || requester - limit->first >= limit->count || requester >= (usize{1} << 24))
        return returned(STATUS_DENIED);
    for (usize n = 3; n < 6; ++n) if (inv.trap.arg(n)) return returned(STATUS_BAD_ARGS);
    constexpr auto usage = object::pool<io::Space>::slot_usage();
    auto admission = check_create<io::Space>(inv, *pool, usage);
    if (!admission) return returned(admission.error());
    auto fee = reserve(*pool, usage);
    auto quota = pool->reference();
    auto owner = ctl->reference();
    if (!fee || !quota || !owner) return returned(STATUS_NO_MEMORY);
    auto typed = std::move(*owner).as<io::Iommu>();
    if (!typed) return returned(STATUS_INVALID_CAP);
    auto object = construct<io::Space>(inv, std::move(*fee), inv.cpu.objects.pmm(),
        inv.cpu.objects.work(), std::move(*quota), std::move(*typed), u32(requester));
    if (!object) return returned(STATUS_NO_MEMORY);
    const auto status = object->get().open(object->reference(), *ctl, inv.target->cpu_domain());
    if (status != STATUS_OK) return returned(status);
    const auto rights = cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect,
        cap::Right::Map, cap::Right::Unmap, cap::Right::Observe, cap::Right::Close, cap::Right::Destroy, cap::Right::Adopt, cap::Right::Reassign, cap::Right::Revoke);
    const auto caps = std::array<cap::View, 1>{cap::View{rights}};
    auto root = object->root(inv.cpu.grants, std::move(*admission), caps[0]);
    if (!root) return returned(STATUS_NO_MEMORY);
    return publication(publish(inv, *object, *root, *pool, caps));
}

template <usize op> [[gnu::noinline]] [[nodiscard]] auto vspace_create(Call& inv) noexcept -> Result {
    auto pool = resolve_pool(inv, cap::Right::Create);
    if (!pool) {
        return returned(cap_status(pool.error()));
    }
    auto admission = check_create<mm::VSpace>(inv, pool.value(), object::pool<mm::VSpace>::slot_usage());
    if (!admission) return returned(admission.error());
    auto fee = reserve(pool.value(), object::pool<mm::VSpace>::slot_usage());
    if (!fee) return returned(pool_error(fee.error()));
    auto object = construct<mm::VSpace>(inv, std::move(*fee), inv.cpu.objects.pmm(),
                            inv.kspace, inv.cpu.objects.work());
    if (!object) return returned(mm::status(object.error()));
    const cap::VmLimit data{
        mm::VRange{mm::va_t{mm::UserBegin}, mm::UserEnd - mm::UserBegin},
        mm::Perms::of(mm::Perm::Read, mm::Perm::Write, mm::Perm::Execute)};
    const auto rights =
        cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Reserve,
                        cap::Right::Delegate, cap::Right::Map, cap::Right::Unmap, cap::Right::Protect,
                        cap::Right::Inspect, cap::Right::Manage, cap::Right::Destroy, cap::Right::Adopt, cap::Right::Reassign, cap::Right::Revoke);
    const auto caps = std::array<cap::View, 1>{cap::View{rights, data}};
    auto root = object->root(inv.cpu.grants, std::move(*admission), caps[0]);
    if (!root) return returned(STATUS_NO_MEMORY);
    return publication(publish(inv, *object, *root, pool.value(), caps));
}

template <usize op> [[gnu::noinline]] [[nodiscard]] auto cspace_create(Call& inv) noexcept -> Result {
    arch::TrapCtx& trap = inv.trap;
    auto pool = resolve_pool(inv, cap::Right::Create);
    if (!pool) {
        return returned(cap_status(pool.error()));
    }
    const cap::CSpace::Quota quota{.slots = trap.arg(1), .pages = trap.arg(2)};
    if (quota.slots == 0 || quota.pages == 0) {
        return returned(STATUS_BAD_ARGS);
    }
    const auto rights = cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect,
                                        cap::Right::Manage, cap::Right::Destroy, cap::Right::Adopt, cap::Right::Reassign, cap::Right::Revoke);
    auto admission = check_create<cap::CSpace>(inv, pool.value(), object::pool<cap::CSpace>::slot_usage());
    if (!admission) return returned(admission.error());
    auto fee = reserve(pool.value(), object::pool<cap::CSpace>::slot_usage());
    if (!fee) return returned(pool_error(fee.error()));
    auto object = construct<cap::CSpace>(inv, std::move(*fee), inv.cpu.objects.pmm(), quota);
    if (!object) return returned(STATUS_NO_MEMORY);

    const auto caps = std::array<cap::View, 1>{cap::View{rights}};
    auto root = object->root(inv.cpu.grants, std::move(*admission), caps[0]);
    if (!root) return returned(STATUS_NO_MEMORY);
    return publication(publish(inv, *object, *root, pool.value(), caps));
}

template <usize op> [[gnu::noinline]] [[nodiscard]] auto domain_create(Call& inv) noexcept -> Result {
    auto quota = resolve_pool(inv, cap::Right::Create);
    auto parent = inv.cspace.resolve<sched::Domain>(handle_of(inv.trap.arg(1)), cap::Rights::of(cap::Right::Create));
    if (!quota || !parent) return returned(cap_status(!quota ? quota.error() : parent.error()));
    const auto budget = inv.cpu.dispatcher().clock().ticks<time::ns>(inv.trap.arg(2));
    const auto period = inv.cpu.dispatcher().clock().ticks<time::ns>(inv.trap.arg(3));
    if (!budget || !period || inv.trap.arg(4) > 65535) return returned(STATUS_BAD_ARGS);
    const sched::Domain::Config config{.cpus=inv.trap.arg(5), .budget=*budget, .period=*period,
        .weight=static_cast<u32>(inv.trap.arg(4))};
    if (!sched::Domain::valid_config(config)) return returned(STATUS_BAD_ARGS);
    auto admission = check_create<sched::Domain>(inv, *quota, object::pool<sched::Domain>::slot_usage());
    if (!admission) return returned(admission.error());
    auto fee = reserve(*quota, object::pool<sched::Domain>::slot_usage());
    if (!fee) return returned(pool_error(fee.error()));
    auto object = construct<sched::Domain>(inv, std::move(*fee), inv.cpu.objects.pmm(), *inv.cpu.cpus, config);
    if (!object) return returned(STATUS_NO_MEMORY);
    const auto opened = object->get().open();
    if (opened != STATUS_OK) return returned(opened);
    const auto caps = std::array{cap::View{cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect, cap::Right::Create,
        cap::Right::Close, cap::Right::Control, cap::Right::Destroy, cap::Right::Adopt, cap::Right::Reassign, cap::Right::Revoke)}};
    auto root = object->root(inv.cpu.grants, std::move(*admission), caps[0]);
    if (!root) return returned(STATUS_NO_MEMORY);
    const auto attached = object->get().attach(*parent);
    if (attached != STATUS_OK) return returned(attached);
    return publication(publish(inv, *object, *root, *quota, caps));
}

template <usize op> [[gnu::noinline]] [[nodiscard]] auto sc_create(Call& inv) noexcept -> Result {
    arch::TrapCtx& trap = inv.trap;
    auto pool = resolve_pool(inv, cap::Right::Create);
    auto domain =
        inv.cspace.resolve<sched::Domain>(handle_of(trap.arg(1)), cap::Rights::of(cap::Right::Create));
    if (!pool || !domain) {
        return returned(cap_status(!pool ? pool.error() : domain.error()));
    }
    const auto budget = inv.cpu.dispatcher().clock().ticks<time::ns>(trap.arg(2));
    const auto period = inv.cpu.dispatcher().clock().ticks<time::ns>(trap.arg(3));
    const auto weight = trap.arg(4);
    const auto cpus = trap.arg(5);
    const CpuId home{cpus ? static_cast<usize>(std::countr_zero(cpus)) : MaxCpus};
    if (!budget || !period || weight > 65535 || !cpus
        || !domain.value()->allows(CpuId{63U - std::countl_zero(cpus)})) {
        return returned(STATUS_BAD_ARGS);
    }
    const sched::Sc::Config config{.budget = *budget, .period = *period, .cpus = cpus, .weight = static_cast<u32>(weight)};
    if (!sched::Sc::valid_config(config)) {
        return returned(STATUS_BAD_ARGS);
    }
    const auto rights = sc_rights();
    auto admission = check_create<sched::Sc>(inv, pool.value(), object::pool<sched::Sc>::slot_usage());
    if (!admission) return returned(admission.error());
    auto fee = reserve(pool.value(), object::pool<sched::Sc>::slot_usage());
    if (!fee) return returned(pool_error(fee.error()));
    auto object = construct<sched::Sc>(inv, std::move(*fee), config, inv.cpu.dispatcher().clock().rd_ticks());
    if (!object) return returned(STATUS_NO_MEMORY);
    auto& obj = object->get();

    const auto caps = std::array<cap::View, 1>{cap::View{rights}};
    auto root = object->root(inv.cpu.grants, std::move(*admission), caps[0]);
    if (!root) return returned(STATUS_NO_MEMORY);
    auto admitted = obj.admit(domain.value(), home);
    if (!admitted)
        return returned(admitted.error() == sched::Sc::Error::WrongCpu ? STATUS_BAD_ARGS
                                                                       : STATUS_BUSY);
    return publication(publish(inv, *object, *root, pool.value(), caps));
}

struct ThreadStart final {
    arch::UserStart user{};
    IpcBinding ipc{};
};

[[nodiscard]] static auto start_snapshot(Call& inv, cap::Handle handle, usize offset) noexcept
    -> std::expected<ThreadStart, status_t> {
    auto snapshot = read_desc<ThreadInit>(inv, handle, offset);
    if (!snapshot) {
        return std::unexpected(snapshot.error());
    }
    const ThreadInit& desc = snapshot.value();
    if (desc.version != THREAD_START_VERSION || desc.flags != 0) {
        return std::unexpected(STATUS_BAD_ARGS);
    }
    arch::UserStart start{
        .entry = desc.entry,
        .stack = desc.stack,
    };
    for (usize index = 0; index < 6; ++index) {
        start.arguments[index] = desc.arguments[index];
    }
    if (!arch::valid_user_start(start)) {
        return std::unexpected(STATUS_BAD_ARGS);
    }
    return (ThreadStart{start, desc.ipc});
}

[[gnu::noinline]] [[nodiscard]] static auto
publish_thread(Call& inv, cap::Resolved<resource::Quota>& pool,
               cap::Resolved<mm::VSpace>& vspace, cap::Resolved<cap::CSpace>& cspace, ThreadStart&& start,
               std::optional<mm::View>&& ipc) noexcept -> Result {
    auto address_space = vspace.reference();
    auto capability_space = cspace.reference();
    if (!address_space || !capability_space) {
        return returned(STATUS_NO_MEMORY);
    }
    auto env =
        Env::user(std::move(address_space).value(), std::move(capability_space).value(), std::move(ipc));
    if (!env) {
        return returned(STATUS_BUSY);
    }

    auto admission = check_create<Thread>(inv, pool, object::pool<Thread>::slot_usage());
    if (!admission) return returned(admission.error());
    auto fee = reserve(pool, object::pool<Thread>::slot_usage());
    if (!fee) return returned(pool_error(fee.error()));
    auto object = construct<Thread>(inv, std::move(*fee), std::move(env).value(), start.user);
    if (!object) return returned(STATUS_NO_MEMORY);
    auto& obj = object->get();
    const auto rights = basic_rights();
    const auto caps = std::array<cap::View, 1>{cap::View{rights}};
    auto root = object->root(inv.cpu.grants, std::move(*admission), caps[0]);
    if (!root) return returned(STATUS_NO_MEMORY);
    if (!obj.bind_roots(vspace, cspace)
        || !obj.claim_home(inv.cpu.cpus->get(CpuId{inv.trap.arg(5)})->dispatcher())) return returned(STATUS_BUSY);
    return publication(publish(inv, *object, *root, pool, caps));
}

template <usize op> [[gnu::noinline]] [[nodiscard]] auto thread_create(Call& inv) noexcept -> Result {
    arch::TrapCtx& trap = inv.trap;
    auto pool = resolve_pool(inv, cap::Right::Create);
    auto vspace = inv.cspace.resolve<mm::VSpace>(handle_of(trap.arg(1)), cap::Rights::of(cap::Right::Manage));
    auto cspace =
        inv.cspace.resolve<cap::CSpace>(handle_of(trap.arg(2)), cap::Rights::of(cap::Right::Manage));
    if (!pool || !vspace || !cspace) {
        const cap::CSpaceError error = !pool ? pool.error() : !vspace ? vspace.error() : cspace.error();
        return returned(cap_status(error));
    }
    auto start = start_snapshot(inv, handle_of(trap.arg(3)), trap.arg(4));
    if (!start || !inv.cpu.cpus->get(CpuId{trap.arg(5)})) {
        return returned(start ? STATUS_BAD_ARGS : start.error());
    }
    auto ipc = prepare_ipc(inv, vspace.value(), start.value().ipc);
    if (!ipc) {
        return returned(ipc.error());
    }
    return publish_thread(inv, pool.value(), vspace.value(), cspace.value(),
                          std::move(start).value(), std::move(ipc).value());
}

template <usize op> [[gnu::noinline]] [[nodiscard]] auto port_create(Call& inv) noexcept -> Result {
    auto pool = resolve_pool(inv, cap::Right::Create);
    if (!pool) return returned(cap_status(pool.error()));
    const ipc::Port::Config config{inv.trap.arg(1), inv.trap.arg(2)};
    const auto bytes = ipc::Port::storage_bytes(config);
    const auto cost = bytes ? add_budget(object::pool<ipc::Port>::slot_usage(), {.memory = *bytes}) : std::nullopt;
    if (!cost) return returned(STATUS_BAD_ARGS);
    auto admission = check_create<ipc::Port>(inv, *pool, *cost);
    if (!admission) return returned(admission.error());
    auto fee = reserve(*pool, object::pool<ipc::Port>::slot_usage());
    if (!fee) return returned(pool_error(fee.error()));
    auto object = construct<ipc::Port>(inv, std::move(*fee), inv.cpu.objects.pmm(), config);
    if (!object || object->get().open() != STATUS_OK) return returned(STATUS_NO_MEMORY);
    const auto rights = cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect,
        cap::Right::Receive, cap::Right::Manage, cap::Right::Destroy, cap::Right::Adopt, cap::Right::Reassign, cap::Right::Revoke);
    const std::array<cap::View, 1> views{cap::View{rights}};
    auto root = object->root(inv.cpu.grants, std::move(*admission), views[0]);
    if (!root) return returned(STATUS_NO_MEMORY);
    return publication(publish(inv, *object, *root, *pool, views));
}

template <usize op> [[gnu::noinline]] [[nodiscard]] auto notification_create(Call& inv) noexcept -> Result {
    auto pool = resolve_pool(inv, cap::Right::Create);
    const u64 badge = inv.trap.arg(1);
    if (!pool) {
        return returned(cap_status(pool.error()));
    }
    if (badge == 0) {
        return returned(STATUS_BAD_ARGS);
    }
    auto admission =
        check_create<ipc::Notification>(inv, pool.value(), object::pool<ipc::Notification>::slot_usage());
    if (!admission) return returned(admission.error());
    auto fee = reserve(pool.value(), object::pool<ipc::Notification>::slot_usage());
    if (!fee) return returned(pool_error(fee.error()));
    auto object = construct<ipc::Notification>(inv, std::move(*fee));
    if (!object) return returned(STATUS_NO_MEMORY);
    const auto rights =
        cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect, cap::Right::Signal,
                        cap::Right::Receive, cap::Right::Destroy, cap::Right::Adopt, cap::Right::Reassign, cap::Right::Revoke);
    const cap::Badge lim{badge};
    const auto caps = std::array<cap::View, 1>{cap::View{rights, lim}};
    auto root = object->root(inv.cpu.grants, std::move(*admission), caps[0]);
    if (!root) return returned(STATUS_NO_MEMORY);
    return publication(publish(inv, *object, *root, pool.value(), caps));
}

#define INST_Create(entry, nr) template auto entry<nr>(Call&) noexcept -> Result;
#define INST_Call(entry, nr)
#define INST_Ipc(entry, nr)
#define CALL(name, nr, entry, unit) INST_##unit(entry, nr)
#include <uapi/calls.def>
#undef CALL
#undef INST_Call
#undef INST_Create
#undef INST_Ipc

} // namespace syscall
