#include "kernel/syscall/internal.hpp"

#include <cap/rights.hpp>
#include <execution/vproc.hpp>
#include <ipc/tunnel.hpp>
#include <object/tunnel_pool.hpp>
#include <uapi/syscall.h>

namespace kernel::syscall {
namespace {

[[nodiscard]] auto status(ipc::TunnelError error) noexcept -> myos_status_t {
    switch (error) {
    case ipc::TunnelError::Closed:
        return MYOS_STATUS_CLOSED;
    case ipc::TunnelError::Busy:
        return MYOS_STATUS_BUSY;
    case ipc::TunnelError::AlreadyConnected:
        return MYOS_STATUS_ALREADY_CONNECTED;
    case ipc::TunnelError::WrongSource:
    case ipc::TunnelError::WrongTarget:
        return MYOS_STATUS_DENIED;
    case ipc::TunnelError::Empty:
        return MYOS_STATUS_RETRY;
    case ipc::TunnelError::BadSequence:
        return MYOS_STATUS_BAD_ARGS;
    case ipc::TunnelError::InvalidSlot:
        return MYOS_STATUS_BAD_ARGS;
    case ipc::TunnelError::GenerationExhausted:
    case ipc::TunnelError::ResourceExhausted:
        return MYOS_STATUS_NO_MEMORY;
    }
    return MYOS_STATUS_INTERNAL;
}

[[nodiscard]] auto resolve(
    Invocation& invocation,
    cap::Right right) noexcept
    -> libk::Expected<cap::Resolved<ipc::Tunnel>, cap::CSpaceError> {
    return invocation.cspace.resolve<ipc::Tunnel>(
        handle_of(invocation.trap.arg(0)), cap::Rights::of(right));
}

} // namespace

auto handle_tunnel(usize operation, Invocation& invocation) noexcept -> Result {
    Vproc* const caller = invocation.target.vproc();
    switch (operation) {
    case MYOS_SYS_TUNNEL_CONNECT: {
        if (caller == nullptr) {
            return returned(MYOS_STATUS_DENIED);
        }
        auto tunnel = resolve(invocation, cap::Right::Connect);
        if (!tunnel) {
            return returned(cap_status(tunnel.error()));
        }
        auto connected = tunnel.value()->connect(
            *caller, invocation.cspace, tunnel.value());
        return connected
            ? returned(MYOS_STATUS_OK, connected.value().raw())
            : returned(status(connected.error()));
    }
    case MYOS_SYS_TUNNEL_INVOKE: {
        if (caller == nullptr) {
            return returned(MYOS_STATUS_DENIED);
        }
        auto tunnel = resolve(invocation, cap::Right::Signal);
        if (!tunnel) {
            return returned(cap_status(tunnel.error()));
        }
        auto invoked = tunnel.value()->invoke(*caller);
        return invoked
            ? returned(MYOS_STATUS_OK, invoked.value())
            : returned(status(invoked.error()));
    }
    case MYOS_SYS_TUNNEL_ACK: {
        if (caller == nullptr) {
            return returned(MYOS_STATUS_DENIED);
        }
        auto tunnel = resolve(invocation, cap::Right::Ack);
        if (!tunnel) {
            return returned(cap_status(tunnel.error()));
        }
        auto acknowledged = tunnel.value()->ack(
            *caller, invocation.trap.arg(1));
        if (!acknowledged) {
            return returned(status(acknowledged.error()));
        }
        const ipc::TunnelAck result = acknowledged.value();
        return returned(
            result.reasserted
                ? MYOS_STATUS_REASSERTED
                : MYOS_STATUS_OK,
            result.sequence);
    }
    case MYOS_SYS_TUNNEL_CLOSE: {
        auto tunnel = resolve(invocation, cap::Right::Close);
        if (!tunnel) {
            return returned(cap_status(tunnel.error()));
        }
        if (caller == nullptr) {
            return returned(MYOS_STATUS_DENIED);
        }
        auto closed = tunnel.value()->close_from(
            *caller, tunnel.value().rights());
        return closed
            ? returned(MYOS_STATUS_OK)
            : returned(status(closed.error()));
    }
    default:
        return returned(MYOS_STATUS_INVALID_OP);
    }
}

} // namespace kernel::syscall
