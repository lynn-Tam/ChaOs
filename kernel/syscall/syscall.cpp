#include <syscall/syscall.hpp>

#include "kernel/syscall/internal.hpp"

#include <cpu/cpu_local.hpp>
#include <sched/dispatcher.hpp>
#include <thread/thread.hpp>
#include <execution/vproc.hpp>
#include <syscall/policy.hpp>
#include <uapi/status.h>
#include <uapi/syscall.h>

namespace kernel::syscall {
namespace {

void publish(arch::TrapContext& context, const Result& result) noexcept {
    context.set_result(
        0, static_cast<usize>(static_cast<isize>(result.status)));
    context.set_result(1, result.value);
    context.set_result(2, result.value2);
}

} // namespace

auto handle(arch::TrapContext& context) noexcept -> Disposition {
    CpuLocal& cpu = current_cpu();
    KASSERT(cpu.dispatcher() != nullptr);
    const execution::Target target = cpu.dispatcher()->current();
    cap::CSpace* const cspace = cpu.cspace();
    kernel::mm::VSpace* const vspace = cpu.vspace();
    KASSERT(target && cspace != nullptr && vspace != nullptr);

    if (Thread* const thread = target.thread()) {
        thread->note_user_syscall();
    }
    context.complete_syscall();
    Invocation invocation{cpu, target, *cspace, *vspace, context};
    const usize operation = context.arg(7);

    const Policy contract = policy(operation);
    if (contract.continuation == Continuation::Invalid
        || (target.thread() != nullptr && !contract.allows_thread())
        || (target.vproc() != nullptr && !contract.allows_vproc())) {
        publish(context, returned(MYOS_STATUS_INVALID_OP));
        return Disposition::Return;
    }

    Result outcome{};
    if (operation <= MYOS_SYS_EXECUTION_START) {
        outcome = handle_execution(operation, invocation);
    } else if (operation >= MYOS_SYS_CAP_CLOSE
        && operation <= MYOS_SYS_OBJECT_DESTROY) {
        outcome = handle_capability(operation, invocation);
    } else if (operation >= MYOS_SYS_VM_MAP
        && operation <= MYOS_SYS_VM_DESTROY_REGION) {
        outcome = handle_vm(operation, invocation);
    } else if (operation >= MYOS_SYS_RESOURCE_CREATE_CHILD
        && operation <= MYOS_SYS_ENDPOINT_CREATE) {
        outcome = handle_construction(operation, invocation);
    } else if (operation >= MYOS_SYS_MEMORY_SEAL
        && operation <= MYOS_SYS_RESOURCE_CLOSE) {
        outcome = handle_object(operation, invocation);
    } else if (operation >= MYOS_SYS_NOTIFICATION_SIGNAL
        && operation <= MYOS_SYS_NOTIFICATION_UNBIND_VPROC) {
        outcome = handle_notification(operation, invocation);
    } else if (operation >= MYOS_SYS_VPROC_ARM
        && operation <= MYOS_SYS_VPROC_PARK) {
        outcome = handle_vproc(operation, invocation);
    } else if (operation >= MYOS_SYS_TUNNEL_CONNECT
        && operation <= MYOS_SYS_TUNNEL_CLOSE) {
        outcome = handle_tunnel(operation, invocation);
    } else if (operation >= MYOS_SYS_ENDPOINT_CALL
        && operation <= MYOS_SYS_ENDPOINT_MINT) {
        outcome = handle_endpoint(operation, invocation);
    } else {
        outcome = returned(MYOS_STATUS_INVALID_OP);
    }
    if (outcome.disposition != Disposition::Resume) {
        publish(context, outcome);
    }
    return outcome.disposition;
}

} // namespace kernel::syscall
