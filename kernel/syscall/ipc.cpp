#include <expected>
#include <optional>
#include <syscall/call.hpp>
#include <cap/cap.hpp>
#include <cpu/local.hpp>
#include <cpu/runtime.hpp>
#include <ipc/notification.hpp>
#include <object/ref.hpp>
#include <task/thread.hpp>
#include <uapi/abi.h>
#include <utility>
#include <state.hpp>
#include <ipc/endpoint.hpp>
#include <sched/dispatcher.hpp>
#include <uapi/ipc.h>
#include <cap/cspace.hpp>
#include <ipc/channel.hpp>
#include <ipc/buffer.hpp>
#include <cpu/registry.hpp>

namespace syscall {

[[nodiscard]] static auto status(ipc::NotificationError error) noexcept
    -> status_t {
    switch (error) {
    case ipc::NotificationError::Closed:
        return STATUS_CLOSED;
    case ipc::NotificationError::Empty:
        return STATUS_RETRY;
    case ipc::NotificationError::Busy:
        return STATUS_BUSY;
    case ipc::NotificationError::InvalidBadge:
        return STATUS_BAD_ARGS;
    }
    return STATUS_INTERNAL;
}

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
    const auto* const authority = std::get_if<cap::Badge>(
        &effective.data);
    if (authority == nullptr || authority->badge == 0) {
        return returned(STATUS_INTERNAL);
    }
    return returned(notification.value()->signal(authority->badge)
        ? STATUS_OK
        : STATUS_CLOSED);
}

template<usize op>
[[nodiscard]] auto notification_take(Call& inv) noexcept -> Result {
    auto notification = resolve(inv, cap::Right::Receive);
    if (!notification) {
        return returned(cap_status(notification.error()));
    }
    auto badges = notification.value()->take();
    return badges
        ? Result{
              STATUS_OK,
              badges.value().badges,
              Disposition::Return,
              badges.value().sequence}
        : returned(status(badges.error()));
}

template<usize op>
[[nodiscard]] auto notification_wait(Call& inv) noexcept -> Result {
    auto notification = resolve(inv, cap::Right::Receive);
    if (!notification) {
        return returned(cap_status(notification.error()));
    }
    CpuRegistry* const cpus = inv.cpu.runtime().owner_registry;
    libk_assert(cpus != nullptr);
    Thread* const thread = inv.target;
    libk_assert(thread != nullptr);
    if (thread->waiting()) {
        return returned(STATUS_BUSY);
    }
    const auto ticks = inv.trap.arg(1);
    const auto deadline = ticks == 0 ? std::optional<time::Instant>{}
        : std::optional<time::Instant>{time::Instant::from_ticks(ticks)};
    auto self = notification.value().reference();
    if (!self) return returned(STATUS_CLOSED);
    auto* target = &notification.value().object();
    // Storage survives the wait, while the admission lease must not stall revoke.
    notification.value().reset();
    const auto result = target->wait(*thread, *cpus, *inv.cpu.dispatcher(),
                                     deadline);
    return returned(result.status, result.value);
}

[[nodiscard]] static auto endpoint_status(ipc::EndpointError error) noexcept
    -> status_t {
    switch (error) {
    case ipc::EndpointError::Closed:
        return STATUS_CLOSED;
    case ipc::EndpointError::Busy:
    case ipc::EndpointError::QueueFull:
        return STATUS_WOULD_BLOCK;
    case ipc::EndpointError::InvalidConfig:
    case ipc::EndpointError::InvalidCaller:
        return STATUS_BAD_ARGS;
    case ipc::EndpointError::DepthExceeded:
    case ipc::EndpointError::BudgetTooLow:
    case ipc::EndpointError::Denied:
        return STATUS_DENIED;
    case ipc::EndpointError::GenerationExhausted:
        return STATUS_INTERNAL;
    case ipc::EndpointError::TransferFailed:
        return STATUS_TRANSFER_FAILED;
    }
    return STATUS_INTERNAL;
}

template<usize op>
[[nodiscard]] auto endpoint_call(Call& inv) noexcept -> Result {
    Thread* const thread = inv.target;
    if (thread == nullptr) {
        return returned(STATUS_INVALID_OP);
    }
    auto endpoint = inv.cspace.resolve<ipc::Endpoint>(
        handle_of(inv.trap.arg(0)),
        cap::Rights::of(cap::Right::Call));
    if (!endpoint) {
        return returned(cap_status(endpoint.error()));
    }
    const usize arguments[3]{
        inv.trap.arg(1),
        inv.trap.arg(2),
        inv.trap.arg(3),
    };
    std::optional<time::Instant> deadline{};
    if (const u64 timeout_ns = inv.trap.arg(4); timeout_ns != 0) {
        auto& clock = inv.cpu.runtime().kernel->clock();
        const auto duration = clock.duration_from_nanoseconds(timeout_ns);
        const auto expires = duration
            ? clock.now().checked_add(*duration) : std::nullopt;
        if (!expires) {
            return returned(STATUS_BAD_ARGS);
        }
        deadline = *expires;
    }
    auto* const object = &endpoint.value().object();
    auto entered = object->call(
        std::move(endpoint).value(),
        *thread,
        inv.trap,
        *inv.cpu.dispatcher(),
        inv.cpu.runtime().kernel->cpus(),
        arguments,
        deadline);
    if (!entered) {
        return returned(endpoint_status(entered.error()));
    }
    return Result{STATUS_OK, 0, Disposition::Resume};
}

template<usize op>
[[nodiscard]] auto endpoint_reply(Call& inv) noexcept -> Result {
    Thread* const thread = inv.target;
    libk_assert(thread != nullptr);
    ipc::Activation* const frame = thread->activation();
    libk_assert(frame != nullptr);
    auto& activation = *frame;
    const isize status = static_cast<isize>(inv.trap.arg(0));
    const usize value = inv.trap.arg(1);
    auto replied = activation.endpoint().reply(
        *thread,
        inv.trap,
        *inv.cpu.dispatcher(),
        status,
        value);
    return replied
        ? Result{STATUS_OK, 0, Disposition::Resume}
        : returned(endpoint_status(replied.error()));
}

template<usize op>
[[nodiscard]] auto endpoint_abort(Call& inv) noexcept -> Result {
    Thread* const thread = inv.target;
    libk_assert(thread != nullptr);
    ipc::Activation* const frame = thread->activation();
    libk_assert(frame != nullptr);
    auto& activation = *frame;
    auto aborted = activation.endpoint().abort(
        *thread,
        inv.trap,
        *inv.cpu.dispatcher(),
        static_cast<isize>(inv.trap.arg(0)));
    return aborted
        ? Result{STATUS_OK, 0, Disposition::Resume}
        : returned(endpoint_status(aborted.error()));
}

template<usize op>
[[nodiscard]] auto endpoint_close(Call& inv) noexcept -> Result {
    auto endpoint = inv.cspace.resolve<ipc::Endpoint>(
        handle_of(inv.trap.arg(0)),
        cap::Rights::of(cap::Right::Close));
    if (!endpoint) {
        return returned(cap_status(endpoint.error()));
    }
    endpoint.value()->close();
    return returned(STATUS_OK);
}

template<usize op>
[[nodiscard]] auto endpoint_mint(Call& inv) noexcept -> Result {
    const cap::Handle source = handle_of(inv.trap.arg(0));
    auto endpoint = inv.cspace.resolve<ipc::Endpoint>(
        source, cap::Rights::of(cap::Right::Delegate));
    const auto rights = rights_of(inv.trap.arg(4));
    const usize cap_limit = inv.trap.arg(3);
    if (!endpoint || !rights || !rights->contains(cap::Right::Call)
        || cap_limit > ENDPOINT_MAX_CAPS) {
        return returned(!endpoint
            ? cap_status(endpoint.error()) : STATUS_BAD_ARGS);
    }

    const cap::EpLimit data{
        .badge = inv.trap.arg(2),
        .fixed = ~u64{},
        .cap_limit = cap_limit,
    };
    const auto publish = [&](cap::CSpace& dest) noexcept -> Result {
        auto minted = inv.cspace.delegate(
            source,
            dest,
            cap::View{*rights, data},
            cap::View{*rights, data});
        return returned(
            minted ? STATUS_OK : cap_status(minted.error()),
            minted ? minted.value().raw() : 0);
    };
    const cap::Handle target = handle_of(inv.trap.arg(1));
    if (!target) {
        return publish(inv.cspace);
    }
    auto dest = inv.cspace.resolve<cap::CSpace>(
        target, cap::Rights::of(cap::Right::Manage));
    return dest
        ? publish(dest.value().object())
        : returned(cap_status(dest.error()));
}

[[nodiscard]] static auto status(ipc::ChannelError error) noexcept -> status_t {
    switch (error) {
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
    case ipc::ChannelError::InvalidRelation:
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

[[nodiscard]] static auto message_buffer(
    Call& inv) noexcept -> ipc::Buffer* {
    return inv.target->ipc_buffer();
}

[[nodiscard]] static auto read_message(
    Call& inv,
    ChanMsg& wire) noexcept -> bool {
    ipc::Buffer* const buffer = message_buffer(inv);
    if (buffer == nullptr) {
        return false;
    }
    return buffer->read(0, libk::Span<byte>{
        reinterpret_cast<byte*>(&wire), sizeof(wire)});
}

template<usize op>
[[nodiscard]] auto channel_send(Call& inv) noexcept -> Result {
    
    const bool blocking = op == SYS_CHANNEL_SEND;
    auto cap = channel(inv, cap::Right::Send);
    if (!cap) {
        return returned(cap_status(cap.error()));
    }
    ChanMsg wire{};
    if (!read_message(inv, wire)
        || wire.version != CHANNEL_VERSION
        || wire.flags != CHANNEL_FLAGS_NONE
        || wire.reserved != 0
        || wire.word_count > CHANNEL_MAX_WORDS
        || wire.cap_count > CHANNEL_MAX_CAPS
        || wire.receive_limit > CHANNEL_MAX_CAPS) {
        return returned(STATUS_BAD_ARGS);
    }
    ipc::ChannelSend request{
        .transaction = wire.transaction,
        .tag = wire.tag,
        .word_count = wire.word_count,
        .cap_count = wire.cap_count,
    };
    for (usize index = 0; index < request.word_count; ++index) {
        request.words[index] = wire.words[index];
    }
    for (usize index = 0; index < request.cap_count; ++index) {
        request.caps[index] = wire.caps[index];
    }
    auto sent = cap.value()->send(
        cap.value(), inv.cspace, request);
    if (sent) {
        return returned(STATUS_OK, sent.value());
    }
    if (!blocking || sent.error() != ipc::ChannelError::WouldBlock) {
        return returned(status(sent.error()));
    }
    Thread* const thread = inv.target;
    CpuRegistry* const cpus = inv.cpu.runtime().owner_registry;
    if (thread == nullptr || cpus == nullptr) {
        return returned(STATUS_INVALID_OP);
    }
    auto* ch = &cap.value().object();
    const auto handle = handle_of(inv.trap.arg(0));
    ipc::Channel::Wait turn{*ch};
    auto ready = ch->wait(std::move(cap).value(), turn, ipc::Channel::Wait::Kind::Send,
        *thread, *cpus);
    if (!ready) return returned(status(ready.error()));
    cap = inv.cspace.resolve<ipc::Channel>(handle, cap::Rights::of(cap::Right::Send));
    if (!cap) return returned(cap_status(cap.error()));
    sent = ch->send(cap.value(), inv.cspace, request, &turn);
    return sent ? returned(STATUS_OK, sent.value()) : returned(status(sent.error()));
}

template<usize op>
[[nodiscard]] auto channel_recv(Call& inv) noexcept -> Result {
    
    const bool blocking = op == SYS_CHANNEL_RECV;
    auto cap = channel(inv, cap::Right::Receive);
    if (!cap) {
        return returned(cap_status(cap.error()));
    }
    ipc::Buffer* const buffer = message_buffer(inv);
    if (buffer == nullptr) {
        return returned(STATUS_BAD_ARGS);
    }
    // Keep the admission lease through dequeue, capability publication, and
    // wire-result publication. A second access here would leave a commit
    // window in which the IPC view could be revoked after the message was
    // consumed but before the result was written.
    auto admitted = buffer->access();
    if (!admitted) {
        return returned(STATUS_BAD_ARGS);
    }
    ChanMsg wire{};
    if (!admitted.value().read(0, libk::Span<byte>{
            reinterpret_cast<byte*>(&wire), sizeof(wire)})
        || wire.version != CHANNEL_VERSION
        || wire.flags != CHANNEL_FLAGS_NONE
        || wire.reserved != 0
        || wire.receive_limit > CHANNEL_MAX_CAPS) {
        return returned(STATUS_BAD_ARGS);
    }
    ipc::ChannelRecv result{
        .receive_limit = wire.receive_limit,
    };
    ipc::Channel::Wait turn{cap.value().object()};
    auto received = cap.value()->receive(
        cap.value(), inv.cspace, result);
    if (!received) {
        if (!blocking || received.error() != ipc::ChannelError::WouldBlock) {
            return returned(status(received.error()));
        }
        Thread* const thread = inv.target;
        CpuRegistry* const cpus = inv.cpu.runtime().owner_registry;
        if (thread == nullptr || cpus == nullptr) {
            return returned(STATUS_INVALID_OP);
        }
        auto* ch = &cap.value().object();
        const auto handle = handle_of(inv.trap.arg(0));
        // Do not pin the IPC mapping while waiting indefinitely for a message.
        admitted = std::unexpected(ipc::BufferError::Unavailable);
        auto ready = ch->wait(std::move(cap).value(), turn, ipc::Channel::Wait::Kind::Receive,
            *thread, *cpus);
        if (!ready) return returned(status(ready.error()));
        cap = inv.cspace.resolve<ipc::Channel>(handle, cap::Rights::of(cap::Right::Receive));
        if (!cap) return returned(cap_status(cap.error()));
        admitted = buffer->access();
        if (!admitted) return returned(STATUS_BAD_ARGS);
        received = ch->receive(cap.value(), inv.cspace, result, &turn);
        if (!received) return returned(status(received.error()));
    }
    wire.transaction = result.transaction;
    wire.tag = result.tag;
    wire.word_count = result.word_count;
    wire.cap_count = result.cap_count;
    wire.sender_badge = result.sender_badge;
    wire.sequence = result.sequence;
    wire.received_count = result.cap_count;
    for (usize index = 0; index < result.word_count; ++index) {
        wire.words[index] = result.words[index];
    }
    for (usize index = 0; index < result.cap_count; ++index) {
        wire.received[index] = result.caps[index].raw();
    }
    if (!admitted.value().write(0, libk::Span<const byte>{
            reinterpret_cast<const byte*>(&wire), sizeof(wire)})) {
        return returned(STATUS_BAD_ARGS);
    }
    return returned(STATUS_OK, result.sequence);
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
[[nodiscard]] auto channel_bind(Call& inv) noexcept -> Result {
    auto condition = static_cast<ipc::ChannelCondition>(
        inv.trap.arg(2));
    if (condition != ipc::ChannelCondition::Readable
        && condition != ipc::ChannelCondition::Writable
        && condition != ipc::ChannelCondition::PeerClosed) {
        return returned(STATUS_BAD_ARGS);
    }
    const cap::Right right = condition == ipc::ChannelCondition::Writable
        ? cap::Right::Send : cap::Right::Receive;
    auto cap = channel(inv, right);
    auto notification = inv.cspace.resolve<ipc::Notification>(
        handle_of(inv.trap.arg(1)), cap::Rights::of(cap::Right::Signal));
    if (!cap || !notification) {
        return returned(!cap
            ? cap_status(cap.error())
            : cap_status(notification.error()));
    }
    auto bound = cap.value()->bind(
        cap.value(), notification.value(), condition);
    return bound
        ? returned(STATUS_OK, bound.value())
        : returned(status(bound.error()));
}

template<usize op>
[[nodiscard]] auto channel_arm(Call& inv) noexcept -> Result {
    auto cap = channel(inv, cap::Right::Receive);
    if (!cap) {
        cap = inv.cspace.resolve<ipc::Channel>(
            handle_of(inv.trap.arg(0)), cap::Rights::of(cap::Right::Send));
    }
    if (!cap) {
        return returned(cap_status(cap.error()));
    }
    auto armed = cap.value()->arm(
        cap.value(), inv.trap.arg(1), inv.trap.arg(2));
    return armed ? returned(STATUS_OK, armed.value()) : returned(status(armed.error()));
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


#define INST_Ipc(entry, nr) template auto entry<nr>(Call&) noexcept -> Result;
#define INST_Call(entry, nr)
#define INST_Create(entry, nr)
#define CALL(name, nr, entry, locus, unit) INST_##unit(entry, nr)
#include <uapi/calls.def>
#undef CALL
#undef INST_Call
#undef INST_Create
#undef INST_Ipc

} // namespace syscall
