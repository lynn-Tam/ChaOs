#include "kernel/syscall/internal.hpp"

#include <execution/vproc.hpp>
#include <operation/completion.hpp>
#include <uapi/syscall.h>

namespace kernel::syscall {
namespace {

[[nodiscard]] auto error_status(VprocError error) noexcept -> myos_status_t {
    switch (error) {
    case VprocError::InvalidRuntime:
        return MYOS_STATUS_BAD_ARGS;
    case VprocError::InvalidState:
        return MYOS_STATUS_BUSY;
    case VprocError::TableFull:
        return MYOS_STATUS_NO_MEMORY;
    case VprocError::InvalidKey:
    case VprocError::GenerationExhausted:
        return MYOS_STATUS_NOT_FOUND;
    }
    return MYOS_STATUS_INTERNAL;
}

} // namespace

auto handle_vproc(usize operation, Invocation& invocation) noexcept -> Result {
    Vproc* const vproc = invocation.target.vproc();
    if (vproc == nullptr) {
        return returned(MYOS_STATUS_INVALID_OP);
    }
    switch (operation) {
    case MYOS_SYS_VPROC_RETURN: {
        auto resumed = vproc->resume(invocation.trap, invocation.trap.arg(0));
        return resumed
            ? Result{MYOS_STATUS_OK, 0, Disposition::Resume}
            : returned(error_status(resumed.error()));
    }
    case MYOS_SYS_VPROC_POLL:
        return returned(MYOS_STATUS_OK, vproc->pending_sequence());
    case MYOS_SYS_OPERATION_TAKE: {
        auto result = vproc->take_operation(
            operation::Key{invocation.trap.arg(0)});
        return result
            ? returned(result.value().status, result.value().value)
            : returned(error_status(result.error()));
    }
    default:
        return returned(MYOS_STATUS_INVALID_OP);
    }
}

} // namespace kernel::syscall
