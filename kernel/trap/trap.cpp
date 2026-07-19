// kernel/trap/trap.cpp
// 系统 trap policy 的当前 owner；架构层只提供 Event 和返回现场访问。

#include "kernel/trap/dump.hpp"

#include <cpu/cpu_local.hpp>
#include <cpu/ipi.hpp>
#include <cpu/cpu_registry.hpp>
#include <cpu/cpu_runtime.hpp>
#include <diag/console.hpp>
#include <mm/vspace.hpp>
#include <operation/wait.hpp>
#include <sched/dispatcher.hpp>
#include <syscall/syscall.hpp>
#include <thread/thread.hpp>
#include <execution/vproc.hpp>
#include <trap/trap.hpp>
#include <uapi/status.h>

namespace kernel::trap {

void handle(const Event& event, arch::TrapContext& context) noexcept {
    if (const auto* interrupt = event.interrupt()) {
        kernel::CpuLocal& cpu = kernel::current_cpu();
        KASSERT(cpu.dispatcher() != nullptr);
        switch (interrupt->cause) {
        case Interrupt::Timer:
            cpu.dispatcher()->on_timer();
            return;
        case Interrupt::Software:
            kernel::handle_ipi(cpu.runtime());
            return;
        default:
            panic_unhandled(event, context);
        }
    }

    if (event.origin() == Origin::User) {
        const auto* exception = event.exception();
        KASSERT(exception != nullptr);
        kernel::CpuLocal& cpu = kernel::current_cpu();
        kernel::Execution* const execution = cpu.current_execution();
        kernel::Thread* const thread = cpu.current_thread();
        kernel::Vproc* const vproc = cpu.current_vproc();
        KASSERT(execution != nullptr && (thread != nullptr || vproc != nullptr)
            && execution->binding().user_bound());
        if (exception->cause == Exception::Syscall) {
            switch (kernel::syscall::handle(context)) {
            case kernel::syscall::Disposition::Return:
            case kernel::syscall::Disposition::Resume:
                return;
            case kernel::syscall::Disposition::Yield:
                cpu.dispatcher()->request_reschedule(
                    kernel::sched::DispatchReason::Yield);
                return;
            case kernel::syscall::Disposition::Block:
                cpu.dispatcher()->request_reschedule(
                    kernel::sched::DispatchReason::Block);
                return;
            case kernel::syscall::Disposition::Park:
                cpu.dispatcher()->request_reschedule(
                    kernel::sched::DispatchReason::Park);
                return;
            case kernel::syscall::Disposition::Exit:
                if (vproc != nullptr) {
                    vproc->request_exit();
                } else {
                    cpu.dispatcher()->request_reschedule(
                        kernel::sched::DispatchReason::Exit);
                }
                return;
            }
        }
        if (exception->cause == Exception::PageFault) {
            kernel::mm::Access access{};
            switch (exception->access) {
            case Access::Read:
                access = kernel::mm::Access::Read;
                break;
            case Access::Write:
                access = kernel::mm::Access::Write;
                break;
            case Access::Execute:
                access = kernel::mm::Access::Execute;
                break;
            case Access::None:
                if (thread != nullptr) {
                    thread->record_user_fault(event);
                    cpu.dispatcher()->request_reschedule(
                        kernel::sched::DispatchReason::Exit);
                } else {
                    vproc->request_exit();
                }
                return;
            }
            auto fault = execution->binding().vspace()->fault(
                kernel::mm::VmContext{
                    .cpus = cpu.runtime().owner_registry,
                    .local = cpu.descriptor->logical_id(),
                },
                kernel::mm::VirtAddr{event.fault_addr()},
                access);
            if (fault && fault.value().kind == kernel::mm::FaultKind::Materialized) {
                return;
            }
            if (fault && fault.value().kind == kernel::mm::FaultKind::Busy) {
                // Another VSpace transaction or page materialization owns the
                // canonical mutation slot. The faulting instruction has not
                // completed and its TrapFrame remains intact, so reschedule
                // and retry the same access after the owner makes progress.
                cpu.dispatcher()->request_reschedule(
                    kernel::sched::DispatchReason::Yield);
                return;
            }
            KASSERT(!fault || fault.value().kind != kernel::mm::FaultKind::Ready);
        }
        KASSERT(execution->binding().fault_route()
            == kernel::FaultRoute::Terminate);
        if (kernel::execution::Frame* const frame =
                thread != nullptr ? thread->active_frame() : nullptr;
            frame != nullptr) {
            frame->unwind(
                context, *cpu.dispatcher(), MYOS_STATUS_PEER_FAULT);
            return;
        }
        if (thread != nullptr) {
            thread->record_user_fault(event);
            kernel::diag::console::print<
                "user: contained fault address={:#x} after syscalls={} "
                "active-vspace-cpus={}\n">(
                event.fault_addr(), thread->user_syscalls(),
                execution->binding().vspace()->active_cpus().size());
        } else {
            kernel::diag::console::print<
                "vproc: contained fault address={:#x} "
                "active-vspace-cpus={}\n">(
                event.fault_addr(),
                execution->binding().vspace()->active_cpus().size());
        }
        if (vproc != nullptr) {
            vproc->request_exit();
        } else {
            cpu.dispatcher()->request_reschedule(
                kernel::sched::DispatchReason::Exit);
        }
        return;
    }

    if (const auto* exception = event.exception()) {
        switch (exception->cause) {
        case Exception::Breakpoint:
            context.complete_breakpoint();
            return;
        default:
            panic_unhandled(event, context);
        }
    }

    panic_unhandled(event, context);
}

void on_exit([[maybe_unused]] arch::TrapContext& context) noexcept {
    kernel::CpuLocal& cpu = kernel::current_cpu();
    KASSERT(cpu.dispatcher() != nullptr);
    kernel::Thread* const thread = cpu.current_thread();
    kernel::Vproc* const vproc = cpu.current_vproc();
    kernel::Execution* const execution = cpu.current_execution();
    KASSERT(execution != nullptr && (thread != nullptr || vproc != nullptr));
    while (thread != nullptr && thread->active_frame() != nullptr
        && (cpu.dispatcher()->current().stop_requested()
            || thread->cancel_pending())) {
        thread->active_frame()->unwind(
            context, *cpu.dispatcher(), MYOS_STATUS_CANCELED);
    }
    operation::Wait* wait = thread != nullptr
        ? &thread->current_wait()
        : nullptr;
    if (wait != nullptr && wait->ready()) {
        wait->finish(context);
    }
    cpu.dispatcher()->on_trap_exit();
    // A wake credit closes wake-before-block, but it is not evidence that the
    // current subsystem operation has completed: an older credit may merely
    // make the first block attempt a no-op. The canonical wait is the truth.
    // Keep this continuation in the kernel until that relation is complete.
    KASSERT(cpu.current_execution() == execution);
    wait = thread != nullptr ? &thread->current_wait() : nullptr;
    while (wait != nullptr && wait->attached()) {
        if (wait->ready()) {
            wait->finish(context);
            break;
        }
        cpu.dispatcher()->block_current();
        KASSERT(cpu.current_execution() == execution);
        wait = thread != nullptr ? &thread->current_wait() : nullptr;
    }
    // A stop request deliberately waits for the subsystem continuation. Once
    // the relation is detached, give the dispatcher one final commit point.
    cpu.dispatcher()->on_trap_exit();
    KASSERT(cpu.current_execution() == execution);
    if (vproc != nullptr) {
        vproc->on_trap_exit(context);
    }
}

} // namespace kernel::trap
