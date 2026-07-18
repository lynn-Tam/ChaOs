#include "kernel/syscall/internal.hpp"

#include <core/kernel_state.hpp>
#include <cpu/cpu_local.hpp>
#include <cpu/cpu_runtime.hpp>
#include <libk/utility.hpp>
#include <object/sched_pool.hpp>
#include <object/thread_pool.hpp>
#include <object/vproc_pool.hpp>
#include <sched/context.hpp>
#include <sched/dispatcher.hpp>
#include <thread/thread.hpp>
#include <execution/vproc.hpp>
#include <uapi/syscall.h>

namespace kernel::syscall {
namespace {

template<typename T>
[[nodiscard]] auto bind_target(
    cap::Resolved<kernel::sched::SchedulingContext>& context,
    cap::Resolved<T>& target) noexcept -> Result {
    auto reference = target.reference();
    if (!reference) {
        return returned(MYOS_STATUS_BUSY);
    }
    auto hold = libk::move(reference).value().template into_hold<T>();
    if (!hold) {
        return returned(MYOS_STATUS_BUSY);
    }
    auto bound = context->bind_authorized(
        libk::move(hold).value(), context, target);
    return returned(bound ? MYOS_STATUS_OK : MYOS_STATUS_BUSY);
}

[[nodiscard]] auto bind(Invocation& invocation) noexcept -> Result {
    auto context = invocation.cspace.resolve<kernel::sched::SchedulingContext>(
        handle_of(invocation.trap.arg(0)),
        cap::Rights::of(cap::Right::Control));
    if (!context) {
        return returned(cap_status(context.error()));
    }
    const cap::CapHandle target = handle_of(invocation.trap.arg(1));
    auto thread = invocation.cspace.resolve<kernel::Thread>(
        target,
        cap::Rights::of(cap::Right::Control));
    if (thread) {
        return bind_target(context.value(), thread.value());
    }
    if (thread.error() != cap::CSpaceError::WrongKind) {
        return returned(cap_status(thread.error()));
    }
    auto vproc = invocation.cspace.resolve<kernel::Vproc>(
        target, cap::Rights::of(cap::Right::Control));
    if (!vproc) {
        return returned(cap_status(vproc.error()));
    }
    return bind_target(context.value(), vproc.value());
}

template<typename T>
[[nodiscard]] auto start_target(
    Invocation& invocation,
    cap::Resolved<T>& target) noexcept -> Result {
    kernel::sched::Binding* const binding = target->binding();
    if (target->state() != T::State::Prepared
        || binding == nullptr || !binding->context().startable()) {
        return returned(MYOS_STATUS_BUSY);
    }
    KernelState* const kernel = invocation.cpu.runtime().kernel;
    KASSERT(kernel != nullptr);
    auto started = kernel::sched::start(kernel->cpus(), *binding);
    return returned(started ? MYOS_STATUS_OK : MYOS_STATUS_BUSY);
}

[[nodiscard]] auto start(Invocation& invocation) noexcept -> Result {
    const cap::CapHandle target = handle_of(invocation.trap.arg(0));
    auto thread = invocation.cspace.resolve<kernel::Thread>(
        target, cap::Rights::of(cap::Right::Control));
    if (thread) {
        return start_target(invocation, thread.value());
    }
    if (thread.error() != cap::CSpaceError::WrongKind) {
        return returned(cap_status(thread.error()));
    }
    auto vproc = invocation.cspace.resolve<kernel::Vproc>(
        target, cap::Rights::of(cap::Right::Control));
    return vproc
        ? start_target(invocation, vproc.value())
        : returned(cap_status(vproc.error()));
}

} // namespace

auto handle_execution(
    usize operation,
    Invocation& invocation) noexcept -> Result {
    switch (operation) {
    case MYOS_SYS_YIELD:
        return Result{MYOS_STATUS_OK, 0, Disposition::Yield};
    case MYOS_SYS_EXIT:
        return Result{MYOS_STATUS_OK, 0, Disposition::Exit};
    case MYOS_SYS_SC_BIND:
        return bind(invocation);
    case MYOS_SYS_EXECUTION_START:
        return start(invocation);
    default:
        return returned(MYOS_STATUS_INVALID_OP);
    }
}

} // namespace kernel::syscall
