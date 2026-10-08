// kernel/trap/trap.cpp
// 系统 trap policy 的当前 owner；架构层只提供 Event 和返回现场访问。

#include <panic.hpp>

#include <cpu/cpu.hpp>
#include <cpu/ipi.hpp>
#include <console.hpp>
#include <irq/irq.hpp>
#include <mm/vspace.hpp>
#include <mm/mem.hpp>
#include <wait.hpp>
#include <sched/dispatcher.hpp>
#include <syscall/call.hpp>
#include <task/thread.hpp>
#include <ipc/endpoint.hpp>
#if TEST_ENABLED
#include <test/scenario.hpp>
#endif
#include <trap/trap.hpp>
#include <uapi/abi.h>
#include <libk/scope_guard.hpp>

namespace trap {

static void finish_thread_page_fault(
    Thread& thread,
    mm::FaultKind kind,
    mm::Virt address,
    mm::Perm access,
    arch::TrapCtx& context,
    sched::Dispatcher& dispatcher) noexcept {
    status_t status{STATUS_PEER_FAULT};
    switch (kind) {
    case mm::FaultKind::Ready:
    case mm::FaultKind::Materialized:
        return;
    case mm::FaultKind::Busy:
        dispatcher.request_reschedule(sched::DispatchReason::Yield);
        return;
    case mm::FaultKind::Pending:
        libk_assert(false);
        return;
    case mm::FaultKind::ResourceExhausted:
    case mm::FaultKind::OutOfMemory: {
        status = STATUS_NO_MEMORY;
        break;
    }
    default:
        break;
    }
    if (ipc::Activation* const frame = thread.activation();
        frame != nullptr) {
        frame->unwind(context, dispatcher, status);
        return;
    }
    Perm trap_access{Perm::None};
    switch (access) {
    case mm::Perm::Read:
        trap_access = Perm::Read;
        break;
    case mm::Perm::Write:
        trap_access = Perm::Write;
        break;
    case mm::Perm::Execute:
        trap_access = Perm::Execute;
        break;
    }
    const Event event = Event::exception(
        Origin::User,
        Exception::PageFault,
        trap_access,
        context.pc(),
        address.raw());
    thread.record_user_fault(event);
    static_cast<void>(thread.exit().claim(
        Exit::Reason::Fault,
        status,
        0,
        event.pc(),
        event.fault_addr()));
    // One-way diagnostic projection; it must never participate in fault policy.
    console::print<
        "user: contained fault pc={:#x} address={:#x} after syscalls={} "
        "active-vspace-cpus={} fault-kind={}\n">(
        event.pc(), event.fault_addr(), thread.user_syscalls(),
        thread.env().vspace()->active_cpus().size(),
        static_cast<u8>(kind));
    dispatcher.request_reschedule(sched::DispatchReason::Exit);
}

void handle(const Event& event, arch::TrapCtx& context) noexcept {
    if (const auto* interrupt = event.interrupt()) {
        Cpu& cpu = current_cpu();
        switch (interrupt->cause) {
        case Interrupt::Timer:
            cpu.dispatcher().on_timer();
            return;
        case Interrupt::Software:
            handle_ipi(cpu);
            return;
        case Interrupt::External: {
            libk_assert(cpu.ext_irq);
            cpu.ext_irq();
            return;
        }
        default:
            panic("unhandled trap", &context);
        }
    }

    if (event.origin() == Origin::User) {
        const auto* exception = event.exception();
        libk_assert(exception != nullptr);
        Cpu& cpu = current_cpu();
        Thread* const thread = cpu.current;
        libk_assert(thread != nullptr
            && thread->env().user_bound());
        if (exception->cause == Exception::Syscall) {
            switch (syscall::handle(context)) {
            case syscall::Disposition::Return:
            case syscall::Disposition::Resume:
                return;
            case syscall::Disposition::Yield:
                cpu.dispatcher().request_reschedule(
                    sched::DispatchReason::Yield);
                return;
            case syscall::Disposition::Exit:
                const status_t status = static_cast<status_t>(
                    context.arg(1));
                cpu.dispatcher().request_reschedule(sched::DispatchReason::Exit, status);
                return;
            }
        }
        if (exception->cause == Exception::PageFault) {
            mm::Perm access{};
            switch (exception->access) {
            case Perm::Read:
                access = mm::Perm::Read;
                break;
            case Perm::Write:
                access = mm::Perm::Write;
                break;
            case Perm::Execute:
                access = mm::Perm::Execute;
                break;
            case Perm::None:
                if (thread != nullptr) {
                    thread->record_user_fault(event);
                    cpu.dispatcher().request_reschedule(
                        sched::DispatchReason::Exit);
                }
                return;
            }
            if (thread != nullptr) {
                libk_assert(cpu.cpus != nullptr);
                const auto address = mm::Virt{event.fault_addr()};
                const auto kind = thread->env().vspace()->fault(
                    *thread, mm::VmCtx{.cpus = cpu.cpus,
                                       .local = cpu.id}, address, access);
                if (!thread->stop_requested())
                    finish_thread_page_fault(*thread, kind, address, access, context, cpu.dispatcher());
                return;
            }
        }
        if (ipc::Activation* const frame =
                thread != nullptr ? thread->activation() : nullptr;
            frame != nullptr) {
            frame->unwind(
                context, cpu.dispatcher(), STATUS_PEER_FAULT);
            return;
        }
        if (thread != nullptr) {
            thread->record_user_fault(event);
            static_cast<void>(thread->exit().claim(
                Exit::Reason::Fault,
                STATUS_PEER_FAULT,
                0,
                event.pc(),
                event.fault_addr()));
            console::print<
                "user: contained fault pc={:#x} address={:#x} after syscalls={} "
                "active-vspace-cpus={}\n">(
                event.pc(), event.fault_addr(), thread->user_syscalls(),
                thread->env().vspace()->active_cpus().size());
        }
        cpu.dispatcher().request_reschedule(sched::DispatchReason::Exit);
        return;
    }

    if (const auto* exception = event.exception()) {
        switch (exception->cause) {
        case Exception::Breakpoint:
            context.complete_breakpoint();
            return;
        default:
            panic("unhandled trap", &context);
        }
    }

    panic("unhandled trap", &context);
}

void on_exit(const Event& event, arch::TrapCtx& context) noexcept {
    Cpu& cpu = current_cpu();
        Thread* const thread = cpu.current;
    libk_assert(thread != nullptr);
    if (event.origin() == Origin::User && event.exception() != nullptr) {
        thread->enter_kernel();
        const auto leave = libk::on_scope_exit([thread]() noexcept {
            thread->leave_kernel();
        });
        handle(event, context);
    }
    cpu.dispatcher().on_trap_exit();
    libk_assert(cpu.current == thread);
    // A canceled Endpoint frame cannot be popped while its leaf Wait still
    // owns the continuation. Once that relation has completed or canceled,
    // this same owning CPU performs the pending chain unwind.
    while (thread != nullptr && thread->activation() != nullptr
        && !thread->current_wait().attached()
        && (cpu.dispatcher().current()->stop_requested()
            || thread->cancel_pending())) {
        thread->activation()->unwind(
            context, cpu.dispatcher(), STATUS_CANCELED);
    }
    // A stop request deliberately waits for the subsystem continuation. Once
    // the relation is detached, give the dispatcher one final commit point.
    cpu.dispatcher().on_trap_exit();
    libk_assert(cpu.current == thread);

}

void on_return() noexcept {
    if (auto* thread = current_cpu().current) thread->release_calls();
}

} // namespace trap
