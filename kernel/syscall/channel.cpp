#include <object/cspace_pool.hpp>
#include <object/notification_pool.hpp>
#include <object/channel_pool.hpp>
#include "kernel/syscall/internal.hpp"

#include <cpu/cpu_local.hpp>
#include <cpu/cpu_runtime.hpp>
#include <ipc/buffer.hpp>
#include <ipc/channel.hpp>
#include <cpu/cpu_registry.hpp>
#include <thread/thread.hpp>
#include <uapi/channel.h>
#include <uapi/syscall.h>

namespace kernel::syscall {
namespace {

[[nodiscard]] auto status(ipc::ChannelError error) noexcept -> myos_status_t {
    switch (error) {
    case ipc::ChannelError::Closed:
        return MYOS_STATUS_CLOSED;
    case ipc::ChannelError::PeerClosed:
        return MYOS_STATUS_PEER_CLOSED;
    case ipc::ChannelError::WouldBlock:
        return MYOS_STATUS_WOULD_BLOCK;
    case ipc::ChannelError::Denied:
        return MYOS_STATUS_DENIED;
    case ipc::ChannelError::Busy:
        return MYOS_STATUS_BUSY;
    case ipc::ChannelError::ResourceExhausted:
        return MYOS_STATUS_NO_MEMORY;
    case ipc::ChannelError::TransferFailed:
        return MYOS_STATUS_TRANSFER_FAILED;
    case ipc::ChannelError::InvalidRelation:
    case ipc::ChannelError::Invalid:
        return MYOS_STATUS_BAD_ARGS;
    case ipc::ChannelError::GenerationExhausted:
        return MYOS_STATUS_BUSY;
    }
    return MYOS_STATUS_INTERNAL;
}

[[nodiscard]] auto channel(
    Invocation& invocation,
    cap::Right right) noexcept
    -> libk::Expected<cap::Resolved<ipc::Channel>, cap::CSpaceError> {
    return invocation.cspace.resolve<ipc::Channel>(
        handle_of(invocation.trap.arg(0)), cap::Rights::of(right));
}

[[nodiscard]] auto message_buffer(
    Invocation& invocation) noexcept -> ipc::Buffer* {
    return invocation.target.ipc_buffer();
}

[[nodiscard]] auto read_message(
    Invocation& invocation,
    myos_channel_message& wire) noexcept -> bool {
    ipc::Buffer* const buffer = message_buffer(invocation);
    if (buffer == nullptr) {
        return false;
    }
    return buffer->read(0, libk::Span<byte>{
        reinterpret_cast<byte*>(&wire), sizeof(wire)});
}

[[nodiscard]] auto send(
    Invocation& invocation,
    bool blocking) noexcept -> Result {
    auto authority = channel(invocation, cap::Right::Send);
    if (!authority) {
        return returned(cap_status(authority.error()));
    }
    myos_channel_message wire{};
    if (!read_message(invocation, wire)
        || wire.version != MYOS_CHANNEL_VERSION
        || wire.flags != MYOS_CHANNEL_FLAGS_NONE
        || wire.reserved != 0
        || wire.word_count > MYOS_CHANNEL_MAX_WORDS
        || wire.cap_count > MYOS_CHANNEL_MAX_CAPS
        || wire.receive_limit > MYOS_CHANNEL_MAX_CAPS) {
        return returned(MYOS_STATUS_BAD_ARGS);
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
    auto sent = authority.value()->send(
        authority.value(), invocation.cspace, request);
    if (sent) {
        return returned(MYOS_STATUS_OK, sent.value());
    }
    if (!blocking || sent.error() != ipc::ChannelError::WouldBlock) {
        return returned(status(sent.error()));
    }
    Thread* const thread = invocation.target.thread();
    CpuRegistry* const cpus = invocation.cpu.runtime().owner_registry;
    if (thread == nullptr || cpus == nullptr) {
        return returned(MYOS_STATUS_INVALID_OP);
    }
    auto started = authority.value()->send_blocking(
        authority.value(),
        handle_of(invocation.trap.arg(0)),
        invocation.cspace,
        *thread,
        *cpus,
        request);
    if (!started) {
        return returned(status(started.error()));
    }
    if (started.value().state == kernel::operation::State::Waiting) {
        return Result{MYOS_STATUS_OK, 0, Disposition::Block};
    }
    if (thread->waiting()) {
        static_cast<void>(thread->resume_wait(invocation.trap));
        return returned(
            static_cast<myos_status_t>(invocation.trap.arg(0)),
            invocation.trap.arg(1));
    }
    return returned(MYOS_STATUS_OK, started.value().value);
}

[[nodiscard]] auto receive(
    Invocation& invocation,
    bool blocking) noexcept -> Result {
    auto authority = channel(invocation, cap::Right::Receive);
    if (!authority) {
        return returned(cap_status(authority.error()));
    }
    ipc::Buffer* const buffer = message_buffer(invocation);
    if (buffer == nullptr) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    // Keep the admission lease through dequeue, capability publication, and
    // wire-result publication. A second access here would leave a commit
    // window in which the IPC view could be revoked after the message was
    // consumed but before the result was written.
    auto admitted = buffer->access();
    if (!admitted) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    myos_channel_message wire{};
    if (!admitted.value().read(0, libk::Span<byte>{
            reinterpret_cast<byte*>(&wire), sizeof(wire)})
        || wire.version != MYOS_CHANNEL_VERSION
        || wire.flags != MYOS_CHANNEL_FLAGS_NONE
        || wire.reserved != 0
        || wire.receive_limit > MYOS_CHANNEL_MAX_CAPS) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    ipc::ChannelRecv result{
        .receive_limit = wire.receive_limit,
    };
    auto received = authority.value()->receive(
        authority.value(), invocation.cspace, result);
    if (!received) {
        if (!blocking || received.error() != ipc::ChannelError::WouldBlock) {
            return returned(status(received.error()));
        }
        Thread* const thread = invocation.target.thread();
        CpuRegistry* const cpus = invocation.cpu.runtime().owner_registry;
        if (thread == nullptr || cpus == nullptr) {
            return returned(MYOS_STATUS_INVALID_OP);
        }
        auto started = authority.value()->receive_blocking(
            authority.value(),
            handle_of(invocation.trap.arg(0)),
            invocation.cspace,
            *thread,
            *cpus,
            message_buffer(invocation),
            result);
        if (!started) {
            return returned(status(started.error()));
        }
        if (started.value().state == kernel::operation::State::Waiting) {
            return Result{MYOS_STATUS_OK, 0, Disposition::Block};
        }
        if (thread->waiting()) {
            static_cast<void>(thread->resume_wait(invocation.trap));
            return returned(
                static_cast<myos_status_t>(invocation.trap.arg(0)),
                invocation.trap.arg(1));
        }
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
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    return returned(MYOS_STATUS_OK, result.sequence);
}

[[nodiscard]] auto close(Invocation& invocation) noexcept -> Result {
    auto authority = channel(invocation, cap::Right::Close);
    if (!authority) {
        return returned(cap_status(authority.error()));
    }
    auto closed = authority.value()->close(authority.value());
    return returned(closed ? MYOS_STATUS_OK : status(closed.error()));
}

[[nodiscard]] auto bind(Invocation& invocation) noexcept -> Result {
    auto condition = static_cast<ipc::ChannelCondition>(
        invocation.trap.arg(2));
    if (condition != ipc::ChannelCondition::Readable
        && condition != ipc::ChannelCondition::Writable
        && condition != ipc::ChannelCondition::PeerClosed) {
        return returned(MYOS_STATUS_BAD_ARGS);
    }
    const cap::Right right = condition == ipc::ChannelCondition::Writable
        ? cap::Right::Send : cap::Right::Receive;
    auto authority = channel(invocation, right);
    auto notification = invocation.cspace.resolve<ipc::Notification>(
        handle_of(invocation.trap.arg(1)), cap::Rights::of(cap::Right::Signal));
    if (!authority || !notification) {
        return returned(!authority
            ? cap_status(authority.error())
            : cap_status(notification.error()));
    }
    auto bound = authority.value()->bind(
        authority.value(), notification.value(), condition);
    return bound
        ? returned(MYOS_STATUS_OK, bound.value())
        : returned(status(bound.error()));
}

[[nodiscard]] auto arm(Invocation& invocation) noexcept -> Result {
    auto authority = channel(invocation, cap::Right::Receive);
    if (!authority) {
        authority = invocation.cspace.resolve<ipc::Channel>(
            handle_of(invocation.trap.arg(0)), cap::Rights::of(cap::Right::Send));
    }
    if (!authority) {
        return returned(cap_status(authority.error()));
    }
    auto armed = authority.value()->arm(
        authority.value(), invocation.trap.arg(1), invocation.trap.arg(2));
    return returned(armed ? MYOS_STATUS_OK : status(armed.error()));
}

[[nodiscard]] auto mint(Invocation& invocation) noexcept -> Result {
    auto authority = channel(invocation, cap::Right::Delegate);
    auto destination = invocation.cspace.resolve<cap::CSpace>(
        handle_of(invocation.trap.arg(1)), cap::Rights::of(cap::Right::Manage));
    auto rights = rights_of(invocation.trap.arg(3));
    if (!authority || !destination || !rights) {
        return returned(!authority ? cap_status(authority.error())
            : !destination ? cap_status(destination.error())
            : MYOS_STATUS_BAD_RIGHTS);
    }
    auto installed = authority.value()->mint(
        authority.value(), destination.value().object(),
        invocation.trap.arg(2), *rights);
    return installed
        ? returned(MYOS_STATUS_OK, installed.value().raw())
        : returned(status(installed.error()));
}

} // namespace

auto handle_channel(
    usize operation,
    Invocation& invocation) noexcept -> Result {
    switch (operation) {
    case MYOS_SYS_CHANNEL_TRY_SEND:
        return send(invocation, false);
    case MYOS_SYS_CHANNEL_SEND:
        return send(invocation, true);
    case MYOS_SYS_CHANNEL_TRY_RECV:
        return receive(invocation, false);
    case MYOS_SYS_CHANNEL_RECV:
        return receive(invocation, true);
    case MYOS_SYS_CHANNEL_CLOSE:
        return close(invocation);
    case MYOS_SYS_CHANNEL_BIND:
        return bind(invocation);
    case MYOS_SYS_CHANNEL_ARM:
        return arm(invocation);
    case MYOS_SYS_CHANNEL_MINT:
        return mint(invocation);
    default:
        return returned(MYOS_STATUS_INVALID_OP);
    }
}

} // namespace kernel::syscall
