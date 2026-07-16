#include <syscall/syscall.hpp>

#include "kernel/syscall/internal.hpp"

#include <cpu/cpu_local.hpp>
#include <thread/thread.hpp>
#include <uapi/status.h>
#include <uapi/syscall.h>

namespace kernel::syscall {
namespace {

void publish(arch::TrapContext& context, const Result& result) noexcept {
    context.set_result(
        0, static_cast<usize>(static_cast<isize>(result.status)));
    context.set_result(1, result.value);
}

} // namespace

auto handle(arch::TrapContext& context) noexcept -> Disposition {
    CpuLocal& cpu = current_cpu();
    Thread* const thread = cpu.current_thread();
    cap::CSpace* const cspace = cpu.cspace();
    kernel::mm::VSpace* const vspace = cpu.vspace();
    KASSERT(thread != nullptr && cspace != nullptr && vspace != nullptr);

    thread->note_user_syscall();
    context.complete_syscall();
    Invocation invocation{cpu, *thread, *cspace, *vspace, context};
    const usize operation = context.arg(7);

    Result outcome{};
    if (operation <= MYOS_SYS_EXIT) {
        outcome = handle_thread(operation, invocation);
    } else if (operation >= MYOS_SYS_CAP_CLOSE
        && operation <= MYOS_SYS_OBJECT_DESTROY) {
        outcome = handle_capability(operation, invocation);
    } else if (operation >= MYOS_SYS_VM_MAP
        && operation <= MYOS_SYS_VM_DESTROY_REGION) {
        outcome = handle_vm(operation, invocation);
    } else {
        outcome = returned(MYOS_STATUS_INVALID_OP);
    }
    publish(context, outcome);
    return outcome.disposition;
}

} // namespace kernel::syscall
