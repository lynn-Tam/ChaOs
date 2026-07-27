#include "kernel/syscall/internal.hpp"

#include <execution/vproc.hpp>
#include <fault/terminal.hpp>
#include <ipc/notification.hpp>
#include <object/notification_pool.hpp>
#include <thread/thread.hpp>
#include <uapi/syscall.h>

namespace kernel::syscall {
namespace {

auto query_thread_or_vproc(Invocation& invocation) noexcept -> Result {
    const cap::CapHandle handle = handle_of(invocation.trap.arg(0));
    auto thread = invocation.cspace.resolve<Thread>(
        handle, cap::Rights::of(cap::Right::Observe));
    if (thread) {
        const auto snapshot = thread.value()->terminal().snapshot();
        return returned(
            MYOS_STATUS_OK,
            snapshot.sequence,
            static_cast<usize>(static_cast<isize>(snapshot.status)));
    }
    auto vproc = invocation.cspace.resolve<Vproc>(
        handle, cap::Rights::of(cap::Right::Observe));
    if (vproc) {
        const auto snapshot = vproc.value()->terminal().snapshot();
        return returned(
            MYOS_STATUS_OK,
            snapshot.sequence,
            static_cast<usize>(static_cast<isize>(snapshot.status)));
    }
    return returned(cap_status(thread.error()));
}

auto bind_thread_or_vproc(Invocation& invocation) noexcept -> Result {
    const cap::CapHandle target_handle = handle_of(invocation.trap.arg(0));
    auto notification = invocation.cspace.resolve<ipc::Notification>(
        handle_of(invocation.trap.arg(1)), cap::Rights::of(cap::Right::Signal));
    if (!notification || invocation.trap.arg(2) == 0) {
        return returned(!notification ? cap_status(notification.error())
                                      : MYOS_STATUS_BAD_ARGS);
    }
    auto thread = invocation.cspace.resolve<Thread>(
        target_handle, cap::Rights::of(cap::Right::Observe));
    if (thread) {
        return returned(thread.value()->observe_terminal(
            notification.value().object(), invocation.trap.arg(2))
                ? MYOS_STATUS_OK
                : MYOS_STATUS_BUSY);
    }
    auto vproc = invocation.cspace.resolve<Vproc>(
        target_handle, cap::Rights::of(cap::Right::Observe));
    if (vproc) {
        return returned(vproc.value()->observe_terminal(
            notification.value().object(), invocation.trap.arg(2))
                ? MYOS_STATUS_OK
                : MYOS_STATUS_BUSY);
    }
    return returned(cap_status(thread.error()));
}

} // namespace

auto handle_terminal(usize operation, Invocation& invocation) noexcept -> Result {
    return operation == MYOS_SYS_TERMINAL_QUERY
        ? query_thread_or_vproc(invocation)
        : bind_thread_or_vproc(invocation);
}

} // namespace kernel::syscall
