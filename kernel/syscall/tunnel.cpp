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
    case ipc::TunnelError::WrongSource:
    case ipc::TunnelError::WrongTarget:
        return MYOS_STATUS_DENIED;
    case ipc::TunnelError::Empty:
        return MYOS_STATUS_RETRY;
    case ipc::TunnelError::InvalidSlot:
        return MYOS_STATUS_BAD_ARGS;
    case ipc::TunnelError::GenerationExhausted:
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
    case MYOS_SYS_TUNNEL_TAKE: {
        if (caller == nullptr) {
            return returned(MYOS_STATUS_DENIED);
        }
        auto tunnel = resolve(invocation, cap::Right::Wait);
        if (!tunnel) {
            return returned(cap_status(tunnel.error()));
        }
        auto taken = tunnel.value()->take(*caller);
        return taken
            ? returned(MYOS_STATUS_OK, taken.value())
            : returned(status(taken.error()));
    }
    case MYOS_SYS_TUNNEL_CLOSE: {
        auto tunnel = resolve(invocation, cap::Right::Close);
        if (!tunnel) {
            return returned(cap_status(tunnel.error()));
        }
        tunnel.value()->close();
        return returned(MYOS_STATUS_OK);
    }
    default:
        return returned(MYOS_STATUS_INVALID_OP);
    }
}

} // namespace kernel::syscall
