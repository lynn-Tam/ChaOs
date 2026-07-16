#include "kernel/syscall/internal.hpp"

#include <uapi/syscall.h>

namespace kernel::syscall {

auto handle_thread(usize operation, Invocation&) noexcept -> Result {
    switch (operation) {
    case MYOS_SYS_YIELD:
        return Result{MYOS_STATUS_OK, 0, Disposition::Yield};
    case MYOS_SYS_EXIT:
        return Result{MYOS_STATUS_OK, 0, Disposition::Exit};
    default:
        return returned(MYOS_STATUS_INVALID_OP);
    }
}

} // namespace kernel::syscall
