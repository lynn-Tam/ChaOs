#include "kernel/syscall/internal.hpp"

#include <cpu/cpu_local.hpp>
#include <cpu/cpu_runtime.hpp>
#include <object/object_store.hpp>
#include <thread/thread.hpp>
#include <uapi/syscall.h>

namespace kernel::syscall {
namespace {

[[nodiscard]] auto destination(
    cap::CSpace& current,
    cap::CapHandle handle) noexcept
    -> libk::Expected<cap::Resolved<cap::CSpace>, cap::CSpaceError> {
    return current.resolve<cap::CSpace>(
        handle, cap::Rights::of(cap::Right::Manage));
}

template<typename Operation>
[[nodiscard]] auto with_destination(
    cap::CSpace& current,
    cap::CapHandle target,
    Operation&& operation) noexcept -> Result {
    if (!target) {
        auto outcome = operation(current);
        return returned(
            outcome ? MYOS_STATUS_OK : cap_status(outcome.error()),
            outcome ? outcome.value().raw() : 0);
    }
    auto resolved = destination(current, target);
    if (!resolved) {
        return returned(cap_status(resolved.error()));
    }
    auto outcome = operation(resolved.value().object());
    return returned(
        outcome ? MYOS_STATUS_OK : cap_status(outcome.error()),
        outcome ? outcome.value().raw() : 0);
}

} // namespace

auto handle_capability(
    usize operation,
    Invocation& invocation) noexcept -> Result {
    cap::CSpace& cspace = invocation.cspace;
    arch::TrapContext& trap = invocation.trap;
    Thread* const thread = invocation.target.thread();

    if (operation == MYOS_SYS_CAP_CLOSE) {
        auto closed = cspace.close(handle_of(trap.arg(0)));
        return returned(
            closed ? MYOS_STATUS_OK : cap_status(closed.error()));
    }
    if (operation == MYOS_SYS_CAP_DUPLICATE
        || operation == MYOS_SYS_CAP_DELEGATE) {
        const cap::CapHandle source = handle_of(trap.arg(0));
        const cap::CapHandle target = handle_of(trap.arg(1));
        const auto rights = rights_of(trap.arg(2));
        if (!source || !rights) {
            return returned(MYOS_STATUS_BAD_ARGS);
        }
        return with_destination(cspace, target, [&](cap::CSpace& out) {
            return operation == MYOS_SYS_CAP_DUPLICATE
                ? cspace.duplicate(source, out, *rights)
                : cspace.delegate(source, out, *rights);
        });
    }
    if (operation == MYOS_SYS_CAP_MOVE) {
        const cap::CapHandle source = handle_of(trap.arg(0));
        const cap::CapHandle target = handle_of(trap.arg(1));
        if (!source) {
            return returned(MYOS_STATUS_BAD_ARGS);
        }
        return with_destination(cspace, target, [&](cap::CSpace& out) {
            return cspace.move(source, out);
        });
    }
    if (operation == MYOS_SYS_CAP_REVOKE) {
        if (thread == nullptr) {
            return returned(MYOS_STATUS_INVALID_OP);
        }
        if (thread->waiting()) {
            return returned(MYOS_STATUS_BUSY);
        }
        const cap::CapHandle source = handle_of(trap.arg(0));
        if (!source || trap.arg(1) > 1) {
            return returned(MYOS_STATUS_BAD_ARGS);
        }
        KASSERT(invocation.cpu.runtime().owner_registry != nullptr);
        auto started = cspace.revoke(
            source,
            *thread,
            *invocation.cpu.runtime().owner_registry,
            trap.arg(1) != 0);
        if (!started) {
            return returned(cap_status(started.error()));
        }
        if (started.value() == kernel::operation::State::Waiting) {
            return Result{MYOS_STATUS_OK, 0, Disposition::Block};
        }
        thread->resume_wait(trap);
        return returned(MYOS_STATUS_OK);
    }
    if (operation == MYOS_SYS_OBJECT_DESTROY) {
        auto destroyed = cspace.destroy(handle_of(trap.arg(0)));
        return returned(
            destroyed ? MYOS_STATUS_OK : cap_status(destroyed.error()));
    }
    return returned(MYOS_STATUS_INVALID_OP);
}

} // namespace kernel::syscall
