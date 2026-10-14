#include <expected>
#include <array>
#include <optional>
#include <syscall/call.hpp>

#include <cap/cap.hpp>
#include <object/pool.hpp>
#include <cpu/cpu.hpp>
#include <libk/checked_arithmetic.hpp>
#include <libk/inplace_vector.hpp>
#include <utility>
#include <ipc/channel.hpp>
#include <object/ref.hpp>
#include <uapi/ipc.h>
#include <ipc/notification.hpp>
#include <mm/table.hpp>
#include <mm/kspace.hpp>
#include <mm/mem.hpp>
#include <mm/vspace.hpp>
#include <sched/sc.hpp>
#include <sched/domain.hpp>
#include <task/thread.hpp>
#include <uapi/abi.h>
#include <uapi/task.h>
#include <uapi/mem.h>
#include <uapi/cap.h>
#include <mm/pager.hpp>
#include <irq/irq.hpp>
#include <io/space.hpp>

namespace syscall {

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

template <class T, usize N = 1>
static auto check_create(Call& inv, cap::Resolved<object::group>& pool, resource::budget limit) noexcept
    -> std::expected<resource::Reservation, status_t> {
    const auto quota = pool_quota(pool);
    std::optional<resource::budget> publication = cap::Graph::node_charge();
    for (usize i = 0; i < N && publication; ++i)
        publication = add_budget(*publication, inv.cpu.grants.node_charge());
    if (!quota || !(quota->object_kinds & OBJ_BIT(static_cast<u16>(object::kind<T>))) || !quota->budget.contains(limit) ||
        !publication || !quota->budget.contains(*publication))
        return std::unexpected(STATUS_DENIED);
    auto admission = reserve(pool, cap::Graph::node_charge());
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
                    cap::Resolved<object::group>& pool, const std::array<cap::View, N>& views,
                    Bind&& bind) noexcept {
    using Result = std::expected<std::array<cap::Handle, N>, cap::CSpace::PublishError>;
    auto lease = root.acquire();
    if (!lease) return Result{std::unexpected(lease.error())};
    auto handles = inv.cspace.publish(views, [&](cap::View view)
        -> std::expected<cap::GrantRef, cap::CSpace::PublishError> {
        auto fee = reserve(pool, cap::Graph::node_charge());
        if (!fee) return std::unexpected(fee.error());
        auto target = lease->clone_target();
        if (!target) return std::unexpected(cap::GrantError::InvalidState);
        auto grant = inv.cpu.grants.derive(std::move(*fee), *lease, std::move(*target), view);
        if (!grant) return std::unexpected(grant.error());
        return std::move(*grant);
    }, std::forward<Bind>(bind));
    if (handles) object.publish().reset();
    return handles;
}

template<class Pending, usize N>
static auto publish(Call& inv, Pending& object, cap::GrantRef& root,
                    cap::Resolved<object::group>& pool, const std::array<cap::View, N>& views) noexcept {
    return publish(inv, object, root, pool, views, [](auto&, usize) { return true; });
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
    return lim != nullptr && lim->range.contains(range) && lim->perms.contains(access);
}

[[nodiscard]] static auto ipc_error(ipc::BufferError error) noexcept -> status_t {
    switch (error) {
    case ipc::BufferError::Invalid:
        return STATUS_BAD_ARGS;
    case ipc::BufferError::Unavailable:
        return STATUS_BUSY;
    case ipc::BufferError::NoMemory:
        return STATUS_NO_MEMORY;
    }
    return STATUS_INTERNAL;
}

[[nodiscard]] static auto bind_ipc(Call& inv, cap::Resolved<mm::VSpace>& vspace,
                                   cap::Resolved<mm::Mem>& memory,
                                   const IpcBinding& desc) noexcept
    -> std::expected<ipc::Buffer, status_t> {
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
    auto buffer = ipc::Buffer::bind(inv.cpu.objects.pmm(), vspace.object(), std::move(reference).value(),
                                    memory.object(), object, mm::VRange{address, *bytes});
    return buffer ? std::expected<ipc::Buffer, status_t>{(std::move(buffer).value())}
                  : std::unexpected(ipc_error(buffer.error()));
}

[[nodiscard]] static auto prepare_ipc(Call& inv, cap::Resolved<mm::VSpace>& vspace,
                                      const IpcBinding& desc) noexcept
    -> std::expected<std::optional<ipc::Buffer>, status_t> {
    if (desc.pages == 0) {
        if (desc.memory != 0 || desc.page != 0 || desc.address != 0) {
            return std::unexpected(STATUS_BAD_ARGS);
        }
        return (std::optional<ipc::Buffer>{std::nullopt});
    }
    auto memory = inv.cspace.resolve<mm::Mem>(handle_of(desc.memory), cap::Rights::of(cap::Right::Map));
    if (!memory) {
        return std::unexpected(cap_status(memory.error()));
    }
    auto buffer = bind_ipc(inv, vspace, memory.value(), desc);
    if (!buffer) {
        return std::unexpected(buffer.error());
    }
    return (std::optional<ipc::Buffer>{std::move(buffer).value()});
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
        .relation_capacity = inv.trap.arg(4),
    };
    if (config.queue_capacity == 0 || config.queue_capacity > CHANNEL_MAX_QUEUE ||
        config.max_words == 0 || config.max_words > CHANNEL_MAX_WORDS ||
        config.max_caps > CHANNEL_MAX_CAPS || config.relation_capacity == 0 ||
        config.relation_capacity > CHANNEL_MAX_RELATIONS) {
        return returned(STATUS_BAD_ARGS);
    }
    const auto bytes = ipc::Channel::storage_bytes(config);
    const auto cost =
        bytes ? add_budget(object::pool<ipc::Channel>::slot_charge(), resource::budget{.memory = *bytes})
              : std::nullopt;
    if (!cost) return returned(STATUS_BAD_ARGS);
    auto admission = check_create<ipc::Channel, 2>(inv, pool.value(), *cost);
    if (!admission) return returned(admission.error());
    auto fee = reserve(pool.value(), *cost);
    if (!fee) return returned(pool_error(fee.error()));
    auto object = inv.cpu.objects.get<ipc::Channel>().create(std::move(*fee), inv.cpu.objects.pmm(), config);
    if (!object) return returned(STATUS_NO_MEMORY);
    auto& obj = object->get();
    if (!obj.open()) return returned(STATUS_NO_MEMORY);
    const auto rights =
        cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect, cap::Right::Send,
                        cap::Right::Receive, cap::Right::Close, cap::Right::Destroy, cap::Right::Revoke);
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
    auto admission = check_create<Pager>(inv, pool.value(), object::pool<Pager>::slot_charge());
    if (!admission) return returned(admission.error());
    auto fee = reserve(pool.value(), object::pool<Pager>::slot_charge());
    if (!fee) return returned(pool_error(fee.error()));
    auto object = inv.cpu.objects.get<Pager>().create(std::move(*fee));
    if (!object) return returned(STATUS_NO_MEMORY);
    const auto rights =
        cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect, cap::Right::Attach,
                        cap::Right::Serve, cap::Right::Supply, cap::Right::Fail, cap::Right::WritebackAck,
                        cap::Right::Close, cap::Right::Destroy, cap::Right::Revoke);
    const auto caps = std::array<cap::View, 1>{cap::View{rights}};
    auto root = object->root(inv.cpu.grants, std::move(*admission), caps[0]);
    if (!root) return returned(STATUS_NO_MEMORY);
    return publication(publish(inv, *object, *root, pool.value(), caps));
}

template <usize op> [[gnu::noinline]] [[nodiscard]] auto resource_create_child(Call& inv) noexcept -> Result {
    arch::TrapCtx& trap = inv.trap;
    auto pool = resolve_pool(inv, cap::Right::Split);
    if (!pool) {
        return returned(cap_status(pool.error()));
    }
    const resource::budget limit{.memory = trap.arg(1), .caps = trap.arg(2)};
    const u64 kinds = trap.arg(3);
    const auto parent = pool_quota(pool.value());
    if (!parent || !parent->budget.contains(limit) || (kinds & ~parent->object_kinds) != 0 || kinds == 0) {
        return returned(STATUS_DENIED);
    }
    const auto charge = add_budget(object::pool<object::group>::slot_charge(), limit);
    if (!charge) {
        return returned(STATUS_BAD_ARGS);
    }
    const auto rights =
        cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect, cap::Right::Create,
                        cap::Right::Split, cap::Right::Close, cap::Right::Revoke);
    auto admission = check_create<object::group>(inv, pool.value(), limit);
    if (!admission) return returned(admission.error());
    auto fee = reserve(pool.value(), *charge);
    if (!fee) return returned(pool_error(fee.error()));
    auto object = inv.cpu.objects.get<object::group>().create(std::move(*fee), limit);
    if (!object) return returned(STATUS_NO_MEMORY);
    const cap::Quota data{limit, kinds};
    const auto caps = std::array<cap::View, 1>{cap::View{rights, data}};
    auto root = object->root(inv.cpu.grants, std::move(*admission), caps[0]);
    if (!root) return returned(STATUS_NO_MEMORY);
    return publication(publish(inv, *object, *root, pool.value(), caps));
}

static auto publish_memory(Call& inv, cap::Resolved<object::group>& pool, usize size,
                           mm::Perms access, mm::Mem::Config config) noexcept -> Result {
    auto admission = check_create<mm::Mem>(inv, pool, object::pool<mm::Mem>::slot_charge());
    if (!admission) return returned(admission.error());
    auto fee = reserve(pool, object::pool<mm::Mem>::slot_charge());
    if (!fee) return returned(pool_error(fee.error()));
    auto object = inv.cpu.objects.get<mm::Mem>().create(std::move(*fee), inv.cpu.objects.pmm(), size,
                                                       std::move(config));
    if (!object) return returned(mem_status(object.error()));
    const cap::MemLimit data{{0, object->get().page_count()}, access};
    const auto rights = cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate,
        cap::Right::Inspect, cap::Right::Map, cap::Right::Destroy, cap::Right::Manage, cap::Right::Revoke);
    const auto caps = std::array<cap::View, 1>{cap::View{rights, data}};
    auto root = object->root(inv.cpu.grants, std::move(*admission), caps[0]);
    if (!root) return returned(STATUS_NO_MEMORY);
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
    const auto size = inv.trap.arg(1), flags = inv.trap.arg(4);
    const auto access = perms_of(inv.trap.arg(2));
    if (!size || size % mm::page_size || !access || (flags & ~usize{MEMORY_PAGER_PRIVATE}))
        return returned(STATUS_BAD_ARGS);
    auto reference = pager->reference();
    if (!reference) return returned(STATUS_BUSY);
    return publish_memory(inv, *pool, size, *access,
                          mm::PagedCfg{std::move(*reference), *access, bool(flags & MEMORY_PAGER_PRIVATE)});
}
static auto physical_claim(Call& inv, cap::Resolved<object::group>& pool, cap::Right right,
                           usize address, usize bytes) noexcept -> std::expected<mm::Phys::Lease, status_t> {
    auto physical = inv.cspace.resolve<mm::Phys>(handle_of(inv.trap.arg(1)), cap::Rights::of(right));
    if (!physical) return std::unexpected(cap_status(physical.error()));
    const auto pages = mm::Pages::from_aligned_bytes(mm::pa_t{address}, bytes);
    const auto view = physical->view();
    const auto* limit = std::get_if<cap::PhysLimit>(&view.data);
    if (!pages || !limit || !limit->range.contains(*pages)) return std::unexpected(STATUS_DENIED);
    auto owner = physical->reference();
    auto payer = pool.reference();
    if (!owner || !payer) return std::unexpected(STATUS_BUSY);
    auto typed = std::move(*owner).as<mm::Phys>();
    if (!typed) return std::unexpected(STATUS_INVALID_CAP);
    return physical->object().claim(std::move(*typed), *payer, *pages);
}
template <usize op> [[gnu::noinline]] [[nodiscard]] auto memory_create_physical(Call& inv) noexcept -> Result {
    auto pool = resolve_pool(inv, cap::Right::Create);
    if (!pool) return returned(cap_status(pool.error()));
    const auto address = inv.trap.arg(2), size = inv.trap.arg(3);
    const auto access = perms_of(inv.trap.arg(4));
    if (!size || !access || access->contains(mm::Perm::Execute) || inv.trap.arg(5))
        return returned(STATUS_BAD_ARGS);
    auto claim = physical_claim(inv, *pool, cap::Right::Create, address, size);
    if (!claim) return returned(claim.error());
    const auto pages = *mm::Pages::from_aligned_bytes(mm::pa_t{address}, size);
    const mm::Extent extent{{0, pages.page_count()}, pages, *access};
    return publish_memory(inv, *pool, size, *access, mm::PhysCfg{{&extent, 1}, {}, std::move(*claim)});
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
    constexpr auto charge = object::pool<irq::Ctl>::slot_charge();
    auto admission = check_create<irq::Ctl>(inv, *pool, charge);
    if (!admission) return returned(admission.error());
    auto claim = physical_claim(inv, *pool, cap::Right::Control, config->address, config->bytes);
    if (!claim) return returned(claim.error());
    auto fee = reserve(*pool, charge);
    if (!fee) return returned(pool_error(fee.error()));
    auto object = inv.cpu.objects.get<irq::Ctl>().create(std::move(*fee), inv.kspace, *inv.cpu.cpus,
                                inv.cpu.objects.work(), std::move(*claim), *config);
    if (!object) return returned(STATUS_NO_MEMORY);
    const auto status = object->get().open();
    if (status != STATUS_OK) return returned(status);
    const auto rights = cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect,
                                       cap::Right::Create, cap::Right::Destroy, cap::Right::Revoke);
    const auto caps = std::array<cap::View, 1>{cap::View{rights, cap::IndexLimit{1, config->sources}}};
    auto root = object->root(inv.cpu.grants, std::move(*admission), caps[0]);
    if (!root) return returned(STATUS_NO_MEMORY);
    return publication(publish(inv, *object, *root, *pool, caps));
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
    auto admission = check_create<irq::Irq>(inv, *pool, object::pool<irq::Irq>::slot_charge());
    if (!admission) return returned(admission.error());
    auto fee = reserve(*pool, object::pool<irq::Irq>::slot_charge());
    auto owner = controller->reference();
    if (!fee || !owner) return returned(STATUS_NO_MEMORY);
    auto typed = std::move(*owner).as<irq::Ctl>();
    if (!typed) return returned(STATUS_INVALID_CAP);
    auto object = inv.cpu.objects.get<irq::Irq>().create(std::move(*fee), *line, std::move(*typed));
    if (!object) return returned(STATUS_NO_MEMORY);
    const auto rights = cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect,
        cap::Right::Route, cap::Right::Observe, cap::Right::Ack, cap::Right::Destroy, cap::Right::Revoke);
    const auto caps = std::array<cap::View, 1>{cap::View{rights, cap::IrqRoute{u32(id), true}}};
    auto root = object->root(inv.cpu.grants, std::move(*admission), caps[0]);
    if (!root) return returned(STATUS_NO_MEMORY);
    return publication(publish(inv, *object, *root, *pool, caps));
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
    constexpr auto charge = object::pool<io::Iommu>::slot_charge();
    auto admission = check_create<io::Iommu>(inv, *pool, charge);
    if (!admission) return returned(admission.error());
    auto claim = physical_claim(inv, *pool, cap::Right::Control, cfg->address, cfg->bytes);
    if (!claim) return returned(claim.error());
    auto fee = reserve(*pool, charge);
    auto payer = pool->reference();
    auto owner = irq->reference();
    if (!fee || !payer || !owner) return returned(STATUS_NO_MEMORY);
    auto typed = std::move(*owner).as<irq::Ctl>();
    if (!typed) return returned(STATUS_INVALID_CAP);
    auto object = inv.cpu.objects.get<io::Iommu>().create(std::move(*fee), inv.cpu.objects.pmm(), inv.kspace,
        *inv.cpu.cpus, inv.cpu.objects.work(), inv.cpu.dispatcher().clock(), std::move(*payer),
        std::move(*claim), std::move(*typed), *line, *cfg);
    if (!object) return returned(STATUS_NO_MEMORY);
    const auto status = object->get().open();
    if (status != STATUS_OK) return returned(status);
    const auto rights = cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect,
                                       cap::Right::Create, cap::Right::Destroy, cap::Right::Revoke);
    const auto caps = std::array<cap::View, 1>{cap::View{rights, cap::IndexLimit{0, usize{1} << cfg->device_bits}}};
    auto root = object->root(inv.cpu.grants, std::move(*admission), caps[0]);
    if (!root) return returned(STATUS_NO_MEMORY);
    return publication(publish(inv, *object, *root, *pool, caps));
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
    constexpr auto charge = object::pool<io::Space>::slot_charge();
    auto admission = check_create<io::Space>(inv, *pool, charge);
    if (!admission) return returned(admission.error());
    auto fee = reserve(*pool, charge);
    auto payer = pool->reference();
    auto owner = ctl->reference();
    if (!fee || !payer || !owner) return returned(STATUS_NO_MEMORY);
    auto typed = std::move(*owner).as<io::Iommu>();
    if (!typed) return returned(STATUS_INVALID_CAP);
    auto object = inv.cpu.objects.get<io::Space>().create(std::move(*fee), inv.cpu.objects.pmm(),
        inv.cpu.objects.work(), std::move(*payer), std::move(*typed), u32(requester));
    if (!object) return returned(STATUS_NO_MEMORY);
    const auto status = object->get().open(object->reference(), *ctl);
    if (status != STATUS_OK) return returned(status);
    const auto rights = cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect,
        cap::Right::Map, cap::Right::Unmap, cap::Right::Observe, cap::Right::Close, cap::Right::Destroy, cap::Right::Revoke);
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
    auto admission = check_create<mm::VSpace>(inv, pool.value(), object::pool<mm::VSpace>::slot_charge());
    if (!admission) return returned(admission.error());
    auto fee = reserve(pool.value(), object::pool<mm::VSpace>::slot_charge());
    if (!fee) return returned(pool_error(fee.error()));
    auto object = inv.cpu.objects.get<mm::VSpace>().create(std::move(*fee), inv.cpu.objects.pmm(),
                            inv.kspace, inv.cpu.objects.work());
    if (!object) return returned(vm_status(object.error()));
    const cap::VmLimit data{
        mm::VRange{mm::va_t{mm::UserBegin}, mm::UserEnd - mm::UserBegin},
        mm::Perms::of(mm::Perm::Read, mm::Perm::Write, mm::Perm::Execute)};
    const auto rights =
        cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Reserve,
                        cap::Right::Delegate, cap::Right::Map, cap::Right::Unmap, cap::Right::Protect,
                        cap::Right::Inspect, cap::Right::Manage, cap::Right::Destroy, cap::Right::Revoke);
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
                                        cap::Right::Manage, cap::Right::Destroy, cap::Right::Revoke);
    auto admission = check_create<cap::CSpace>(inv, pool.value(), object::pool<cap::CSpace>::slot_charge());
    if (!admission) return returned(admission.error());
    auto fee = reserve(pool.value(), object::pool<cap::CSpace>::slot_charge());
    if (!fee) return returned(pool_error(fee.error()));
    auto object = inv.cpu.objects.get<cap::CSpace>().create(std::move(*fee), inv.cpu.objects.pmm(), quota);
    if (!object) return returned(STATUS_NO_MEMORY);

    const auto caps = std::array<cap::View, 1>{cap::View{rights}};
    auto root = object->root(inv.cpu.grants, std::move(*admission), caps[0]);
    if (!root) return returned(STATUS_NO_MEMORY);
    return publication(publish(inv, *object, *root, pool.value(), caps));
}

template <usize op> [[gnu::noinline]] [[nodiscard]] auto sc_create(Call& inv) noexcept -> Result {
    arch::TrapCtx& trap = inv.trap;
    auto pool = resolve_pool(inv, cap::Right::Create);
    auto domain =
        inv.cspace.resolve<sched::Domain>(handle_of(trap.arg(1)), cap::Rights::of(cap::Right::Control));
    if (!pool || !domain) {
        return returned(cap_status(!pool ? pool.error() : domain.error()));
    }
    const auto budget = inv.cpu.dispatcher().clock().ticks<time::ns>(trap.arg(2));
    const auto period = inv.cpu.dispatcher().clock().ticks<time::ns>(trap.arg(3));
    const auto urgency = sched::Urgency::make(trap.arg(4));
    const CpuId home{trap.arg(5)};
    if (!budget || !period || !urgency) {
        return returned(STATUS_BAD_ARGS);
    }
    const sched::Sc::Config config{.budget = *budget, .period = *period, .urgency = *urgency};
    if (!sched::Sc::valid_config(config)) {
        return returned(STATUS_BAD_ARGS);
    }
    const auto rights = sc_rights();
    auto admission = check_create<sched::Sc>(inv, pool.value(), object::pool<sched::Sc>::slot_charge());
    if (!admission) return returned(admission.error());
    auto fee = reserve(pool.value(), object::pool<sched::Sc>::slot_charge());
    if (!fee) return returned(pool_error(fee.error()));
    auto object = inv.cpu.objects.get<sched::Sc>().create(std::move(*fee), config, inv.cpu.dispatcher().clock().rd_ticks());
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
publish_thread(Call& inv, cap::Resolved<object::group>& pool,
               cap::Resolved<mm::VSpace>& vspace, cap::Resolved<cap::CSpace>& cspace, ThreadStart&& start,
               std::optional<ipc::Buffer>&& ipc) noexcept -> Result {
    auto stack_reservation = reserve(pool, resource::budget{.memory = mm::Stack::StackBytes});
    if (!stack_reservation) {
        return returned(pool_error(stack_reservation.error()));
    }
    auto stack_capacity = std::move(stack_reservation).value();
    auto home = mm::Stack::create(inv.kspace);
    auto address_space = vspace.reference();
    auto capability_space = cspace.reference();
    if (!home || !address_space || !capability_space) {
        return returned(STATUS_NO_MEMORY);
    }
    auto env =
        Env::user(std::move(address_space).value(), std::move(capability_space).value(), std::move(ipc));
    if (!env) {
        return returned(STATUS_BUSY);
    }

    const auto total =
        add_budget(object::pool<Thread>::slot_charge(), resource::budget{.memory = mm::Stack::StackBytes});
    libk_assert(total);
    auto admission = check_create<Thread>(inv, pool, *total);
    if (!admission) return returned(admission.error());
    auto fee = reserve(pool, object::pool<Thread>::slot_charge());
    if (!fee) return returned(pool_error(fee.error()));
    auto object = inv.cpu.objects.get<Thread>().create(std::move(*fee), std::move(stack_capacity).commit(),
                            std::move(home).value(), std::move(env).value(), start.user);
    if (!object) return returned(STATUS_NO_MEMORY);
    auto& obj = object->get();
    const auto rights = basic_rights();
    const auto caps = std::array<cap::View, 1>{cap::View{rights}};
    auto root = object->root(inv.cpu.grants, std::move(*admission), caps[0]);
    if (!root) return returned(STATUS_NO_MEMORY);
    if (!obj.authorize(vspace, cspace)) return returned(STATUS_BUSY);
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
    if (!start || trap.arg(5) != 0) {
        return returned(start ? STATUS_BAD_ARGS : start.error());
    }
    auto ipc = prepare_ipc(inv, vspace.value(), start.value().ipc);
    if (!ipc) {
        return returned(ipc.error());
    }
    return publish_thread(inv, pool.value(), vspace.value(), cspace.value(),
                          std::move(start).value(), std::move(ipc).value());
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
        check_create<ipc::Notification>(inv, pool.value(), object::pool<ipc::Notification>::slot_charge());
    if (!admission) return returned(admission.error());
    auto fee = reserve(pool.value(), object::pool<ipc::Notification>::slot_charge());
    if (!fee) return returned(pool_error(fee.error()));
    auto object = inv.cpu.objects.get<ipc::Notification>().create(std::move(*fee));
    if (!object) return returned(STATUS_NO_MEMORY);
    const auto rights =
        cap::Rights::of(cap::Right::Duplicate, cap::Right::Delegate, cap::Right::Inspect, cap::Right::Signal,
                        cap::Right::Receive, cap::Right::Destroy, cap::Right::Revoke);
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
