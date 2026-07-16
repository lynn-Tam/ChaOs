// kernel/trap/trap.cpp
// 系统 trap policy 的当前 owner；架构层只提供 Event 和返回现场访问。

#include "kernel/trap/dump.hpp"

#include <cpu/cpu_local.hpp>
#include <cpu/ipi.hpp>
#include <cpu/cpu_registry.hpp>
#include <cpu/cpu_runtime.hpp>
#include <diag/console.hpp>
#include <mm/vspace.hpp>
#include <sched/dispatcher.hpp>
#include <syscall/syscall.hpp>
#include <thread/thread.hpp>
#include <trap/trap.hpp>

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
        kernel::Thread* const thread = cpu.current_thread();
        KASSERT(thread != nullptr && thread->execution().user_bound());
        if (exception->cause == Exception::Syscall) {
            switch (kernel::syscall::handle(context)) {
            case kernel::syscall::Disposition::Return:
                return;
            case kernel::syscall::Disposition::Yield:
                cpu.dispatcher()->request_reschedule(
                    kernel::sched::DispatchReason::Yield);
                return;
            case kernel::syscall::Disposition::Block:
                cpu.dispatcher()->request_reschedule(
                    kernel::sched::DispatchReason::Block);
                return;
            case kernel::syscall::Disposition::Exit:
                cpu.dispatcher()->request_reschedule(
                    kernel::sched::DispatchReason::Exit);
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
                thread->record_user_fault(event);
                cpu.dispatcher()->request_reschedule(
                    kernel::sched::DispatchReason::Exit);
                return;
            }
            auto fault = thread->execution().vspace()->fault(
                kernel::mm::VmContext{
                    .cpus = cpu.runtime().owner_registry,
                    .local = cpu.descriptor->logical_id(),
                },
                kernel::mm::VirtAddr{event.fault_addr()},
                access);
            if (fault && fault.value().kind == kernel::mm::FaultKind::Materialized) {
                return;
            }
            KASSERT(!fault || fault.value().kind != kernel::mm::FaultKind::Ready);
        }
        KASSERT(thread->execution().fault_route() == kernel::FaultRoute::Terminate);
        thread->record_user_fault(event);
        kernel::diag::console::print<
            "user: contained fault after syscalls={} active-vspace-cpus={}\n">(
            thread->user_syscalls(),
            thread->execution().vspace()->active_cpus().size());
        cpu.dispatcher()->request_reschedule(
            kernel::sched::DispatchReason::Exit);
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
    cpu.dispatcher()->on_trap_exit();
    kernel::Thread* const thread = cpu.current_thread();
    KASSERT(thread != nullptr);
    if (thread->wait_ready()) {
        thread->resume_wait(context);
    }
}

} // namespace kernel::trap
