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
#include <trap.hpp>
#include <sync.hpp>
#include <trace.hpp>
#include <uapi/abi.h>
#include <libk/scope_guard.hpp>

using Cause = arch::TrapEvent::Cause;
using Perm = arch::TrapEvent::Perm;

static void finish_fault(
    Thread& thread,
    mm::FaultKind kind,
    mm::va_t address,
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
    static_cast<void>(thread.exit().claim(
        Exit::Reason::Fault,
        status,
        0,
        context.pc(),
        address.raw()));
    // One-way diagnostic projection; it must never participate in fault policy.
    console::print<
        "user: contained fault pc={:#x} address={:#x} after syscalls={} "
        "active-vspace-cpus={} fault-kind={}\n">(
        context.pc(), address.raw(), thread.user_syscalls(),
        thread.env().vspace()->active_cpus().size(),
        static_cast<u8>(kind));
    dispatcher.request_reschedule(sched::DispatchReason::Exit);
}

static void handle_trap(const arch::TrapEvent& event, arch::TrapCtx& context) noexcept {
    if (event.irq()) {
        Cpu& cpu = current_cpu();
        switch (event.cause) {
        case Cause::Timer:
            cpu.dispatcher().on_timer();
            return;
        case Cause::Ipi:
            handle_ipi(cpu);
            return;
        case Cause::External: {
            libk_assert(cpu.ext_irq);
            cpu.ext_irq();
            return;
        }
        default:
            panic("unhandled trap", &context);
        }
    }

    if (event.user) {
        Cpu& cpu = current_cpu();
        Thread* const thread = cpu.current;
        libk_assert(thread != nullptr
            && thread->env().user_bound());
        if (event.cause == Cause::Syscall) {
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
        if (event.cause == Cause::PageFault) {
            mm::Perm perm{};
            switch (event.perm) {
            case Perm::Read:
                perm = mm::Perm::Read;
                break;
            case Perm::Write:
                perm = mm::Perm::Write;
                break;
            case Perm::Execute:
                perm = mm::Perm::Execute;
                break;
            case Perm::None:
                libk_assert(false);
                __builtin_unreachable();
            }
            libk_assert(cpu.cpus != nullptr);
            const auto address = mm::va_t{event.fault_addr};
            const auto kind = thread->env().vspace()->fault(
                *thread, mm::VmCtx{.cpus = cpu.cpus,
                                   .local = cpu.id}, address, perm);
            if (!thread->stop_requested())
                finish_fault(*thread, kind, address, context, cpu.dispatcher());
            return;
        }
        if (ipc::Activation* const frame = thread->activation()) {
            frame->unwind(
                context, cpu.dispatcher(), STATUS_PEER_FAULT);
            return;
        }
        static_cast<void>(thread->exit().claim(
            Exit::Reason::Fault,
            STATUS_PEER_FAULT,
            0,
            event.pc,
            event.fault_addr));
        console::print<
            "user: contained fault pc={:#x} address={:#x} after syscalls={} "
            "active-vspace-cpus={}\n">(
            event.pc, event.fault_addr, thread->user_syscalls(),
            thread->env().vspace()->active_cpus().size());
        cpu.dispatcher().request_reschedule(sched::DispatchReason::Exit);
        return;
    }

    if (event.cause == Cause::Breakpoint) {
        context.complete_breakpoint();
        return;
    }

    panic("unhandled trap", &context);
}

static void leave_trap(const arch::TrapEvent& event, arch::TrapCtx& context) noexcept {
    Cpu& cpu = current_cpu();
    Thread* const thread = cpu.current;
    libk_assert(thread != nullptr);
    if (event.user && !event.irq()) {
        thread->enter_kernel();
        const auto leave = libk::on_scope_exit([thread]() noexcept {
            thread->leave_kernel();
        });
        handle_trap(event, context);
    }
    cpu.dispatcher().on_trap_exit();
    libk_assert(cpu.current == thread);
    // A canceled Endpoint frame cannot be popped while its leaf Wait still
    // owns the continuation. Once that relation has completed or canceled,
    // this same owning CPU performs the pending chain unwind.
    while (thread->activation() != nullptr
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

extern "C" auto trap_enter(arch::TrapFrame* f) noexcept -> arch::TrapFrame* {
    libk_assert(f);
    arch::TrapCtx ctx{*f};
    if ((arch::local() && __atomic_load_n(&arch::local()->stop, __ATOMIC_ACQUIRE))) panic_stop(ctx);
    auto event = arch::decode(*f);
    trace::emit(trace::Event::TrapEnter, static_cast<u64>(event.user));
    if (!event.user || event.irq()) handle_trap(event, ctx);
    return ctx.frame();
}
extern "C" auto trap_exit(arch::TrapFrame* f) noexcept -> arch::TrapFrame* {
    libk_assert(f && arch::local()->depth == 0);
    arch::TrapCtx ctx{*f};
    sync::assert_unlocked();
    leave_trap(arch::decode(*f), ctx);
    trace::emit(trace::Event::TrapExit);
    return ctx.frame();
}
extern "C" void trap_return(arch::TrapFrame*) noexcept {
    if (auto* thread = current_cpu().current) thread->release_calls();
}
