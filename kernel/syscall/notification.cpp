#include "kernel/syscall/internal.hpp"

#include <cap/authority.hpp>
#include <cpu/cpu_local.hpp>
#include <cpu/cpu_runtime.hpp>
#include <ipc/notification.hpp>
#include <object/notification_pool.hpp>
#include <thread/thread.hpp>
#include <execution/vproc.hpp>
#include <uapi/syscall.h>

namespace kernel::syscall {
namespace {

[[nodiscard]] auto status(ipc::NotificationError error) noexcept
    -> myos_status_t {
    switch (error) {
    case ipc::NotificationError::Closed:
        return MYOS_STATUS_CLOSED;
    case ipc::NotificationError::Empty:
        return MYOS_STATUS_RETRY;
    case ipc::NotificationError::Busy:
        return MYOS_STATUS_BUSY;
    case ipc::NotificationError::InvalidBadge:
        return MYOS_STATUS_BAD_ARGS;
    }
    return MYOS_STATUS_INTERNAL;
}

[[nodiscard]] auto resolve(
    Invocation& invocation,
    cap::Right right) noexcept
    -> libk::Expected<cap::Resolved<ipc::Notification>, cap::CSpaceError> {
    return invocation.cspace.resolve<ipc::Notification>(
        handle_of(invocation.trap.arg(0)), cap::Rights::of(right));
}

[[nodiscard]] auto signal(Invocation& invocation) noexcept -> Result {
    auto notification = resolve(invocation, cap::Right::Signal);
    if (!notification) {
        return returned(cap_status(notification.error()));
    }
    const cap::EffectiveAuthority effective =
        notification.value().authority();
    const auto* const authority = libk::get_if<cap::NotificationAuthority>(
        &effective.data);
    if (authority == nullptr || authority->badge == 0) {
        return returned(MYOS_STATUS_INTERNAL);
    }
    return returned(notification.value()->signal(authority->badge)
        ? MYOS_STATUS_OK
        : MYOS_STATUS_CLOSED);
}

[[nodiscard]] auto take(Invocation& invocation) noexcept -> Result {
    auto notification = resolve(invocation, cap::Right::Receive);
    if (!notification) {
        return returned(cap_status(notification.error()));
    }
    auto badges = notification.value()->take(invocation.target.vproc());
    return badges
        ? Result{
              MYOS_STATUS_OK,
              badges.value().badges,
              Disposition::Return,
              badges.value().sequence}
        : returned(status(badges.error()));
}

[[nodiscard]] auto wait(Invocation& invocation) noexcept -> Result {
    auto notification = resolve(invocation, cap::Right::Receive);
    if (!notification) {
        return returned(cap_status(notification.error()));
    }
    CpuRegistry* const cpus = invocation.cpu.runtime().owner_registry;
    KASSERT(cpus != nullptr);
    Thread* const thread = invocation.target.thread();
    KASSERT(thread != nullptr);
    if (thread->waiting()) {
        return returned(MYOS_STATUS_BUSY);
    }
    auto started = notification.value()->wait(*thread, *cpus);
    if (!started) {
        return returned(status(started.error()));
    }
    if (started.value().state == operation::State::Waiting) {
        return Result{MYOS_STATUS_OK, 0, Disposition::Block};
    }
    if (thread->waiting()) {
        thread->resume_wait(invocation.trap);
        return returned(
            static_cast<myos_status_t>(invocation.trap.arg(0)),
            invocation.trap.arg(1));
    }
    return returned(MYOS_STATUS_OK, started.value().badges);
}

[[nodiscard]] auto bind_vproc(Invocation& invocation) noexcept -> Result {
    Vproc* const vproc = invocation.target.vproc();
    KASSERT(vproc != nullptr);
    auto notification = resolve(invocation, cap::Right::Receive);
    if (!notification) {
        return returned(cap_status(notification.error()));
    }
    CpuRegistry* const cpus = invocation.cpu.runtime().owner_registry;
    KASSERT(cpus != nullptr);
    auto bound = notification.value()->bind_vproc(
        *vproc,
        *cpus,
        notification.value(),
        invocation.trap.arg(1),
        invocation.trap.arg(2));
    return returned(bound ? MYOS_STATUS_OK : status(bound.error()));
}

[[nodiscard]] auto unbind_vproc(Invocation& invocation) noexcept -> Result {
    Vproc* const vproc = invocation.target.vproc();
    KASSERT(vproc != nullptr);
    auto notification = resolve(invocation, cap::Right::Receive);
    if (!notification) {
        return returned(cap_status(notification.error()));
    }
    auto unbound = notification.value()->unbind_vproc(*vproc);
    return returned(unbound ? MYOS_STATUS_OK : status(unbound.error()));
}

} // namespace

auto handle_notification(
    usize operation,
    Invocation& invocation) noexcept -> Result {
    switch (operation) {
    case MYOS_SYS_NOTIFICATION_SIGNAL:
        return signal(invocation);
    case MYOS_SYS_NOTIFICATION_TAKE:
        return take(invocation);
    case MYOS_SYS_NOTIFICATION_WAIT:
        return wait(invocation);
    case MYOS_SYS_NOTIFICATION_BIND_VPROC:
        return bind_vproc(invocation);
    case MYOS_SYS_NOTIFICATION_UNBIND_VPROC:
        return unbind_vproc(invocation);
    default:
        return returned(MYOS_STATUS_INVALID_OP);
    }
}

} // namespace kernel::syscall
