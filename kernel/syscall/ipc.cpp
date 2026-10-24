#include <expected>
#include <optional>
#include <syscall/call.hpp>
#include <cap/cap.hpp>
#include <cpu/cpu.hpp>
#include <ipc/notification.hpp>
#include <ipc/port.hpp>
#include <object/ref.hpp>
#include <task/thread.hpp>
#include <uapi/abi.h>
#include <utility>
#include <object/pool.hpp>
#include <sched/dispatcher.hpp>
#include <uapi/ipc.h>
#include <cap/cspace.hpp>
#include <ipc/channel.hpp>
#include <mm/vspace.hpp>
#include <io/space.hpp>

namespace syscall {

[[nodiscard]] static auto resolve(
    Call& inv,
    cap::Right right) noexcept
    -> std::expected<cap::Resolved<ipc::Notification>, cap::CSpaceError> {
    return inv.cspace.resolve<ipc::Notification>(
        handle_of(inv.trap.arg(0)), cap::Rights::of(right));
}

template<usize op>
[[nodiscard]] auto notification_signal(Call& inv) noexcept -> Result {
    auto notification = resolve(inv, cap::Right::Signal);
    if (!notification) {
        return returned(cap_status(notification.error()));
    }
    const cap::View effective =
        notification.value().view();
    const auto* const badge = std::get_if<cap::Badge>(
        &effective.data);
    if (badge == nullptr || badge->badge == 0) {
        return returned(STATUS_INTERNAL);
    }
    return returned(notification.value()->signal(badge->badge)
        ? STATUS_OK
        : STATUS_CLOSED);
}

template<usize op>
[[nodiscard]] auto notification_take(Call& inv) noexcept -> Result {
    auto notification = resolve(inv, cap::Right::Receive);
    if (!notification) {
        return returned(cap_status(notification.error()));
    }
    const auto badges = notification.value()->events().take();
    return badges.status == STATUS_OK ? returned(STATUS_OK, badges.value, badges.sequence) : returned(badges.status);
}

[[nodiscard]] static auto status(ipc::ChannelError error) noexcept -> status_t {
    switch (error) {
    case ipc::ChannelError::Abandoned:
        return STATUS_PEER_FAULT;
    case ipc::ChannelError::Canceled:
        return STATUS_CANCELED;
    case ipc::ChannelError::InvalidCap:
        return STATUS_INVALID_CAP;
    case ipc::ChannelError::Closed:
        return STATUS_CLOSED;
    case ipc::ChannelError::PeerClosed:
        return STATUS_PEER_CLOSED;
    case ipc::ChannelError::WouldBlock:
        return STATUS_WOULD_BLOCK;
    case ipc::ChannelError::Denied:
        return STATUS_DENIED;
    case ipc::ChannelError::Busy:
        return STATUS_BUSY;
    case ipc::ChannelError::ResourceExhausted:
        return STATUS_NO_MEMORY;
    case ipc::ChannelError::TransferFailed:
        return STATUS_TRANSFER_FAILED;
    case ipc::ChannelError::Invalid:
        return STATUS_BAD_ARGS;
    case ipc::ChannelError::GenerationExhausted:
        return STATUS_BUSY;
    }
    return STATUS_INTERNAL;
}

[[nodiscard]] static auto channel(
    Call& inv,
    cap::Right right) noexcept
    -> std::expected<cap::Resolved<ipc::Channel>, cap::CSpaceError> {
    return inv.cspace.resolve<ipc::Channel>(
        handle_of(inv.trap.arg(0)), cap::Rights::of(right));
}

template<usize op>
[[nodiscard]] auto channel_send(Call& inv) noexcept -> Result {
    auto cap = channel(inv, cap::Right::Send);
    if (!cap) return returned(cap_status(cap.error()));
    auto* buffer = inv.target->ipc_buffer();
    auto access = buffer ? buffer->access() : std::nullopt;
    ChanMsg wire;
    if (!access || !access->read(0, {reinterpret_cast<byte*>(&wire), offsetof(ChanMsg, words)})
        || wire.version != CHANNEL_VERSION || wire.flags != CHANNEL_FLAGS_NONE || wire.reserved
        || wire.word_count > CHANNEL_MAX_WORDS || wire.cap_count > CHANNEL_MAX_CAPS
        || wire.receive_limit > CHANNEL_MAX_CAPS
        || !access->read(offsetof(ChanMsg, words), {reinterpret_cast<byte*>(wire.words), wire.word_count * sizeof(word_t)})
        || !access->read(offsetof(ChanMsg, caps), {reinterpret_cast<byte*>(wire.caps), wire.cap_count * sizeof(CapXfer)}))
        return returned(STATUS_BAD_ARGS);
    if constexpr (op == SYS_CHANNEL_CALL) {
        auto reply = inv.cspace.resolve<ipc::Channel>(handle_of(inv.trap.arg(1)),
            cap::Rights::of(cap::Right::Send, cap::Right::Delegate));
        if (!reply) return returned(cap_status(reply.error()));
        auto sent = cap.value()->call(*cap, inv.cspace, wire, *reply);
        return sent ? returned(STATUS_OK, *sent) : returned(status(sent.error()));
    }
    const auto sent = cap.value()->send(*cap, inv.cspace, wire);
    return sent ? returned(STATUS_OK, *sent) : returned(status(sent.error()));
}

template<usize op>
[[nodiscard]] auto channel_recv(Call& inv) noexcept -> Result {
    auto cap = channel(inv, cap::Right::Receive);
    if (!cap) return returned(cap_status(cap.error()));
    auto* buffer = inv.target->ipc_buffer();
    auto access = buffer ? buffer->access() : std::nullopt;
    ChanMsg wire;
    if (!access || !access->read(0, {reinterpret_cast<byte*>(&wire), offsetof(ChanMsg, words)})
        || wire.version != CHANNEL_VERSION || wire.flags != CHANNEL_FLAGS_NONE || wire.reserved
        || wire.receive_limit > CHANNEL_MAX_CAPS) return returned(STATUS_BAD_ARGS);
    // One admitted view spans dequeue, cap publication and the result copy.
    const auto received = cap.value()->receive(*cap, inv.cspace, wire);
    if (!received) return returned(status(received.error()),
        received.error() == ipc::ChannelError::ResourceExhausted ? wire.received_count : 0,
        received.error() == ipc::ChannelError::ResourceExhausted ? wire.sequence : 0);
    wire.received_reserved = 0;
    libk_assert(access->write(0, {reinterpret_cast<const byte*>(&wire), offsetof(ChanMsg, words) + wire.word_count * sizeof(word_t)}));
    libk_assert(access->write(offsetof(ChanMsg, received), {reinterpret_cast<const byte*>(wire.received), wire.received_count * sizeof(cap_t)}));
    libk_assert(access->write(offsetof(ChanMsg, received_count), {reinterpret_cast<const byte*>(&wire.received_count), sizeof(ChanMsg) - offsetof(ChanMsg, received_count)}));
    return returned(STATUS_OK, wire.sequence);
}

template<usize op> auto channel_drop(Call& inv) noexcept -> Result {
    auto cap = channel(inv, cap::Right::Receive);
    return cap ? returned(cap->object().drop(*cap, inv.trap.arg(1))) : returned(cap_status(cap.error()));
}

template<usize op> auto channel_cancel(Call& inv) noexcept -> Result {
    if (inv.trap.arg(2) & ~word_t{CHANNEL_DISCARD}) return returned(STATUS_BAD_ARGS);
    auto cap = inv.cspace.resolve<ipc::Channel>(handle_of(inv.trap.arg(0)),
        cap::Rights::of(cap::Right::Receive, cap::Right::Close));
    if (!cap) return returned(cap_status(cap.error()));
    return returned(cap.value()->cancel(*cap, inv.trap.arg(1), inv.trap.arg(2) & CHANNEL_DISCARD)
        ? STATUS_OK : STATUS_STALE);
}

template<usize op>
[[nodiscard]] auto channel_close(Call& inv) noexcept -> Result {
    auto cap = channel(inv, cap::Right::Close);
    if (!cap) {
        return returned(cap_status(cap.error()));
    }
    auto closed = cap.value()->close(cap.value());
    return returned(closed ? STATUS_OK : status(closed.error()));
}

template<usize op>
[[nodiscard]] auto channel_mint(Call& inv) noexcept -> Result {
    auto cap = channel(inv, cap::Right::Delegate);
    auto dest = inv.cspace.resolve<cap::CSpace>(
        handle_of(inv.trap.arg(1)), cap::Rights::of(cap::Right::Manage));
    auto rights = rights_of(inv.trap.arg(3));
    if (!cap || !dest || !rights) {
        return returned(!cap ? cap_status(cap.error())
            : !dest ? cap_status(dest.error())
            : STATUS_BAD_RIGHTS);
    }
    auto installed = cap.value()->mint(
        cap.value(), dest.value().object(),
        inv.trap.arg(2), *rights);
    return installed
        ? returned(STATUS_OK, installed.value().raw())
        : returned(status(installed.error()));
}


// Typed source selection also fixes each capability's observation ceiling.
static auto source(cap::Resolved<void>& cap, u64 facet, u64 mask) noexcept -> Source* {
    auto reference = cap.reference();
    if (!reference) return nullptr;
    if (facet == SOURCE_OP && cap.rights().contains(cap::Right::Inspect)
        && !(mask & ~(OP_ACCEPTED | OP_COMMITTED | OP_DRAINING | OP_DONE))) {
        return object::Types::visit(reference->kind(), [&]<class T>() noexcept -> Source* {
            if constexpr (requires(T& value) { value.op(); }) return &static_cast<T*>(reference->get())->op().events();
            else return nullptr;
        }, static_cast<Source*>(nullptr));
    }
    if (reference->kind() == ObjChannel) {
        const auto view = cap.view();
        const auto* side = std::get_if<cap::ChanLimit>(&view.data);
        if (!side || side->side == cap::ChannelSide::Any || mask & ~(SOURCE_READY | SOURCE_TERMINAL)) return nullptr;
        const bool write = facet == SOURCE_WRITE;
        if ((!write && facet != SOURCE_READ) || !cap.rights().contains(write ? cap::Right::Send : cap::Right::Receive)) return nullptr;
        return &static_cast<ipc::Channel*>(reference->get())->events(side->side, write);
    }
    if (reference->kind() == ObjThread) {
        auto& thread = *static_cast<Thread*>(reference->get());
        if (facet == SOURCE_EXIT && cap.rights().contains(cap::Right::Observe) && !(mask & ~SOURCE_READY))
            return &thread.exit().events();
        if (facet == SOURCE_STATE && cap.rights().contains(cap::Right::Inspect)
            && !(mask & ~(THREAD_ENABLED | THREAD_APPLIED | THREAD_BOUND | THREAD_STOPPING | THREAD_STOPPED)))
            return &thread.events();
        return nullptr;
    }
    if (reference->kind() == ObjVSpace && facet == SOURCE_EXIT && cap.rights().contains(cap::Right::Observe)
        && !(mask & ~(EXEC_FAILED | EXEC_STOPPING | EXEC_STOPPED))) {
        const auto view = cap.view();
        const auto* limit = std::get_if<cap::VmLimit>(&view.data);
        auto& vm = *static_cast<mm::VSpace*>(reference->get());
        return limit && vm.can_destroy_object(*limit) ? &vm.exec_events() : nullptr;
    }
    if (reference->kind() == ObjDomain && facet == SOURCE_STATE && cap.rights().contains(cap::Right::Inspect)
        && !(mask & ~(DOMAIN_CLOSING | DOMAIN_DRAINED | DOMAIN_CONFIGURING)))
        return &static_cast<sched::Domain*>(reference->get())->events();
    if (reference->kind() == ObjSc && facet == SOURCE_STATE && cap.rights().contains(cap::Right::Inspect)
        && !(mask & ~(SC_BOUND | SC_WITHDRAWING)))
        return &static_cast<sched::Sc*>(reference->get())->events();
    if (reference->kind() == ObjPager && facet == SOURCE_READ && cap.rights().contains(cap::Right::Serve)
        && !(mask & ~(SOURCE_READY | SOURCE_TERMINAL)))
        return &static_cast<Pager*>(reference->get())->events();
    if (reference->kind() == ObjIoSpace && facet == SOURCE_STATE && cap.rights().contains(cap::Right::Observe)
        && !(mask & ~((u64{1} << (IO_SPACE_FAILED + 1)) - 1)))
        return &static_cast<io::Space*>(reference->get())->events();
    if (reference->kind() == ObjIrq && facet == SOURCE_READ && cap.rights().contains(cap::Right::Observe)
        && !(mask & ~(SOURCE_READY | SOURCE_TERMINAL)))
        return &static_cast<irq::Irq*>(reference->get())->events();
    if (facet != SOURCE_STATE) return nullptr;
    if (reference->kind() == ObjScope && cap.rights().contains(cap::Right::Inspect)
        && !(mask & ~(STATE_OPEN | STATE_CLOSING | STATE_DRAINED))) {
        const auto view = cap.view();
        const auto* limit = std::get_if<cap::ScopeLimit>(&view.data);
        if (limit && (limit->facets & SCOPE_INFO))
            return &static_cast<object::Scope*>(reference->get())->events();
        return nullptr;
    }
    if (reference->kind() == ObjQuota && cap.rights().contains(cap::Right::Inspect)
        && !(mask & ~(STATE_OPEN | STATE_CLOSING | STATE_DRAINED)))
        return &static_cast<resource::Quota*>(reference->get())->events();
    if (!cap.rights().contains(cap::Right::Receive)) return nullptr;
    if (reference->kind() == ObjNotification)
        return &static_cast<ipc::Notification*>(reference->get())->events();
    if (reference->kind() == ObjPort && !(mask & ~(PORT_READABLE | PORT_OVERFLOW)))
        return &static_cast<ipc::Port*>(reference->get())->events();
    return nullptr;
}

template<usize op> auto port_watch(Call& inv) noexcept -> Result {
    auto port = inv.cspace.resolve<ipc::Port>(handle_of(inv.trap.arg(0)), cap::Rights::of(cap::Right::Manage));
    auto target = inv.cspace.resolve<void>(handle_of(inv.trap.arg(1)), {});
    if (!port || !target) return returned(cap_status(!port ? port.error() : target.error()));
    // Ports aggregate sources and are directly waitable. Nested aggregation
    // would introduce recursive hint publication and source/receiver cycles.
    if (target->pin().kind() == ObjPort && inv.trap.arg(2) != SOURCE_OP) return returned(STATUS_INVALID_OP);
    if (inv.trap.arg(6) & ~WATCH_CHANGED) return returned(STATUS_BAD_ARGS);
    auto* event = source(*target, inv.trap.arg(2), inv.trap.arg(3));
    if (!event) return returned(STATUS_DENIED);
    auto watch = port->object().watch(*port, target->pin(), *event, inv.trap.arg(2), inv.trap.arg(5),
        {.mask = inv.trap.arg(3), .seen = inv.trap.arg(4), .changed = bool(inv.trap.arg(6) & WATCH_CHANGED)}, {});
    return watch ? returned(STATUS_OK, *watch) : returned(watch.error());
}

template<usize op> auto grant_source(Call& inv) noexcept -> Result {
    auto owner = inv.cspace.resolve<object::Scope>(handle_of(inv.trap.arg(0)), cap::Rights::of(cap::Right::Inspect));
    if (!owner) return returned(cap_status(owner.error()));
    const auto view = owner->view();
    const auto* limit = std::get_if<cap::ScopeLimit>(&view.data);
    if (!limit || !(limit->facets & SCOPE_GRANTS)) return returned(STATUS_DENIED);
    const object::Id id{inv.trap.arg(1)};
    auto target = owner->object().lookup(id, limit->kinds, limit->depth);
    if (!target) return returned(STATUS_NOT_FOUND);
    auto space = target.as<cap::CSpace>();
    if (!space) return returned(STATUS_INVALID_OP);
    auto selected = space->get().observe(handle_of(inv.trap.arg(2)), cap::GrantId{inv.trap.arg(3)}, limit->depth);
    if (!selected) return returned(selected.error());
    // A metadata sample cannot follow an object after it leaves this scope.
    if (!owner->object().contains(id, limit->kinds, limit->depth)) return returned(STATUS_STALE);
    auto& source = selected->events();
    if constexpr (op == SYS_GRANT_SOURCE) {
        const auto fact = source.snapshot();
        if (!owner->object().contains(id, limit->kinds, limit->depth)) return returned(STATUS_STALE);
        return returned(fact.status, fact.value, fact.sequence);
    } else {
        auto port = inv.cspace.resolve<ipc::Port>(handle_of(inv.trap.arg(4)), cap::Rights::of(cap::Right::Manage));
        if (!port) return returned(cap_status(port.error()));
        auto watch = port->object().watch(*port, owner->pin(), source, SOURCE_GRANT, inv.trap.arg(6),
            {.mask=0, .seen=inv.trap.arg(5), .changed=true}, {&owner->object(), id, *limit});
        return watch ? returned(STATUS_OK, *watch) : returned(watch.error());
    }
}

template<usize op> auto port_call(Call& inv) noexcept -> Result {
    auto port = inv.cspace.resolve<ipc::Port>(handle_of(inv.trap.arg(0)),
        cap::Rights::of(op == SYS_PORT_ARM || op == SYS_PORT_UNWATCH ? cap::Right::Manage : cap::Right::Receive));
    if (!port) return returned(cap_status(port.error()));
    if constexpr (op == SYS_PORT_ARM) {
        const auto observed = port->object().arm(inv.trap.arg(1), inv.trap.arg(2));
        return returned(observed.status, observed.value, observed.sequence);
    }
    if constexpr (op == SYS_PORT_UNWATCH) return returned(port->object().unwatch(inv.trap.arg(1)));
    if constexpr (op == SYS_PORT_RESYNC) return returned(port->object().resync(inv.trap.arg(1)));
    auto* buffer = inv.target->ipc_buffer();
    if (!buffer) return returned(STATUS_BAD_ARGS);
    auto access = buffer->access();
    if (!access) return returned(STATUS_BAD_ARGS);
    if constexpr (op == SYS_PORT_TAKE) {
        PortEvent event;
        // Result destination is validated before consuming a queued hint.
        if (inv.trap.arg(1) > buffer->size() || sizeof(event) > buffer->size() - inv.trap.arg(1)) return returned(STATUS_BAD_ARGS);
        const auto status = port->object().take(event);
        if (status != STATUS_OK) return returned(status);
        return returned(access->write(inv.trap.arg(1), std::span<const byte>{reinterpret_cast<const byte*>(&event), sizeof(event)}) ? STATUS_OK : STATUS_BAD_ARGS);
    }
    if constexpr (op == SYS_PORT_RECORDS) {
        PortRecords records;
        const auto status = port->object().records(inv.trap.arg(2), inv.trap.arg(3), inv.trap.arg(4), records);
        if (status != STATUS_OK) return returned(status);
        return returned(access->write(inv.trap.arg(1), std::span<const byte>{reinterpret_cast<const byte*>(&records), sizeof(records)}) ? STATUS_OK : STATUS_BAD_ARGS);
    }
    return returned(STATUS_INVALID_OP);
}

template<usize op> auto source_call(Call& inv) noexcept -> Result {
    auto target = inv.cspace.resolve<void>(handle_of(inv.trap.arg(0)), {});
    if (!target) return returned(cap_status(target.error()));
    const auto mask = op == SYS_SOURCE_INFO ? 0 : inv.trap.arg(2);
    auto* event = source(*target, inv.trap.arg(1), mask);
    if (!event) return returned(STATUS_DENIED);
    if constexpr (op == SYS_SOURCE_INFO) {
        const auto fact = event->snapshot();
        return returned(fact.status, fact.value, fact.sequence);
    }
    if (inv.trap.arg(5) & ~WATCH_CHANGED) return returned(STATUS_BAD_ARGS);
    auto storage = target->reference();
    if (!storage) return returned(STATUS_CLOSED);
    const auto deadline = inv.trap.arg(4) ? std::optional<u64>{inv.trap.arg(4)} : std::nullopt;
    std::array<const object::ref<>*, 1> gates{&*storage};
    auto admit = [&](Source::Commit commit, Source::Action revoke, Source::Action ready) noexcept {
        return inv.target->await_grant(target->pin(), gates, commit, revoke, ready);
    };
    auto ready = event->await({*inv.target, *inv.cpu.cpus, inv.cpu.dispatcher()}, {.mask = mask,
        .seen = inv.trap.arg(3), .changed = bool(inv.trap.arg(5) & WATCH_CHANGED)}, deadline,
        Source::Admission::bind(admit));
    target->reset();
    if (ready.status == STATUS_PENDING) return {STATUS_PENDING, 0, Disposition::Park};
    return returned(ready.status, ready.value, ready.sequence);
}

#define INST_Ipc(entry, nr) template auto entry<nr>(Call&) noexcept -> Result;
#define INST_Call(entry, nr)
#define INST_Create(entry, nr)
#define CALL(name, nr, entry, unit) INST_##unit(entry, nr)
#include <uapi/calls.def>
#undef CALL
#undef INST_Call
#undef INST_Create
#undef INST_Ipc

} // namespace syscall
