// kernel/trap/trap.cpp
// 系统 trap policy 的当前 owner；架构层只提供 Event 和返回现场访问。

#include "kernel/trap/dump.hpp"

#include <cpu/cpu_local.hpp>
#include <cpu/ipi.hpp>
#include <cpu/cpu_registry.hpp>
#include <cpu/cpu_runtime.hpp>
#include <arch/uart.hpp>
#include <diag/console.hpp>
#include <diag/concurrency.hpp>
#include <irq/irq.hpp>
#include <mm/vspace.hpp>
#include <mm/virtual_layout.hpp>
#include <operation/page_fault.hpp>
#include <operation/wait.hpp>
#include <sched/dispatcher.hpp>
#include <syscall/syscall.hpp>
#include <thread/thread.hpp>
#include <execution/vproc.hpp>
#include <trap/trap.hpp>
#include <uapi/status.h>

namespace kernel::trap {

namespace {

/*luna change: share initial and resumed Thread page-fault terminal handling, reason: one PageFault owns retry outcome while trap policy owns yield, unwind, and exit decisions*/
void finish_thread_page_fault(
    Thread& thread,
    operation::PageFault& page_fault,
    arch::TrapContext& context,
    sched::CpuDispatcher& dispatcher) noexcept {
    KASSERT(page_fault.terminal());
    const mm::FaultKind kind = page_fault.kind();
    const mm::VirtAddr address = page_fault.address();
    const mm::Access access = page_fault.access();
    page_fault.reset();
    myos_status_t status{MYOS_STATUS_PEER_FAULT};
    switch (kind) {
    case mm::FaultKind::Ready:
    case mm::FaultKind::Materialized:
        return;
    case mm::FaultKind::Busy:
        dispatcher.request_reschedule(sched::DispatchReason::Yield);
        return;
    case mm::FaultKind::Pressure:
        dispatcher.request_reschedule(sched::DispatchReason::Yield);
        return;
    case mm::FaultKind::Pending:
        KASSERT(false);
        return;
    case mm::FaultKind::ResourceExhausted:
    case mm::FaultKind::OutOfMemory: {
        status = MYOS_STATUS_NO_MEMORY;
        break;
    }
    default:
        break;
    }
    KASSERT(thread.effective_binding().fault_route()
        == FaultRoute::Terminate);
    if (execution::Frame* const frame = thread.active_frame();
        frame != nullptr) {
        frame->unwind(context, dispatcher, status);
        return;
    }
    Access trap_access{Access::None};
    switch (access) {
    case mm::Access::Read:
        trap_access = Access::Read;
        break;
    case mm::Access::Write:
        trap_access = Access::Write;
        break;
    case mm::Access::Execute:
        trap_access = Access::Execute;
        break;
    }
    const Event event = Event::exception(
        Origin::User,
        Exception::PageFault,
        trap_access,
        context.pc(),
        address.raw());
    thread.record_user_fault(event);
    static_cast<void>(thread.terminal().claim(
        fault::Reason::Fault,
        status,
        0,
        event.pc(),
        event.fault_addr()));
    kernel::diag::console::print<
        "user: contained fault address={:#x} after syscalls={} "
        "active-vspace-cpus={}\n">(
        event.fault_addr(), thread.user_syscalls(),
        thread.effective_binding().vspace()->active_cpus().size());
    dispatcher.request_reschedule(sched::DispatchReason::Exit);
}

} // namespace

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
        case Interrupt::External: {
            static arch::riscv64::Plic plic{
                kernel::mm::layout::DirectMapBegin
                + arch::riscv64::virt_plic_base};
            const u32 source = plic.claim();
            if (source != 0) {
                kernel::irq::Irq::dispatch(source);
                plic.complete(source);
            }
            return;
        }
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
            /*luna change: route Thread faults through the leaf PageFault continuation, reason: Pager Pending must block on one Wait/Completion instead of polling Yield while Vproc keeps its existing adapter*/
            if (thread != nullptr) {
                KASSERT(cpu.runtime().owner_registry != nullptr);
                auto& page_fault = thread->current_wait().page_fault();
                const mm::FaultKind result = page_fault.start(
                    *execution->binding().vspace(),
                    *cpu.runtime().owner_registry,
                    cpu.descriptor->logical_id(),
                    mm::VirtAddr{event.fault_addr()},
                    access);
                /*luna change: block both pager and retained-pressure waits,
                  reason: pressure wake owns the same Completion rearm lane*/
                if (result == mm::FaultKind::Pending
                    || result == mm::FaultKind::Pressure) {
                    KASSERT(thread->begin_wait(
                        page_fault.completion(),
                        *cpu.runtime().owner_registry));
                    if (page_fault.completion().complete()) {
                        page_fault.completion().signal();
                    }
                    cpu.dispatcher()->request_reschedule(
                        sched::DispatchReason::Block);
                    return;
                }
                finish_thread_page_fault(
                    *thread, page_fault, context, *cpu.dispatcher());
                return;
            }
            /*luna change: route Vproc faults through the durable FaultSlot adapter, reason: Pending must retain the exact return frame while the runtime continues*/
            KASSERT(vproc != nullptr && cpu.runtime().owner_registry != nullptr);
            const mm::FaultKind fault = vproc->fault(
                context,
                *cpu.runtime().owner_registry,
                cpu.descriptor->logical_id(),
                mm::VirtAddr{event.fault_addr()},
                access);
            switch (fault) {
            case mm::FaultKind::Ready:
            case mm::FaultKind::Materialized:
                return;
            case mm::FaultKind::Pending:
                return;
            case mm::FaultKind::Busy:
            case mm::FaultKind::Pressure:
                cpu.dispatcher()->request_reschedule(
                    sched::DispatchReason::Yield);
                return;
            default:
                break;
            }
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
            static_cast<void>(thread->terminal().claim(
                kernel::fault::Reason::Fault,
                MYOS_STATUS_PEER_FAULT,
                0,
                event.pc(),
                event.fault_addr()));
            kernel::diag::console::print<
                "user: contained fault address={:#x} after syscalls={} "
                "active-vspace-cpus={}\n">(
                event.fault_addr(), thread->user_syscalls(),
                execution->binding().vspace()->active_cpus().size());
        } else {
            static_cast<void>(vproc->terminal().claim(
                kernel::fault::Reason::Fault,
                MYOS_STATUS_PEER_FAULT,
                0,
                event.pc(),
                event.fault_addr()));
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
    auto continuation = diag::concurrency::ObservationLease::reserve(
        diag::concurrency::RecordKind::TrapContinuation,
        reinterpret_cast<u64>(execution),
        1,
        diag::concurrency::Expectation::InternalFinite);
    const auto driver = execution->scheduler_binding() != nullptr
        ? execution->scheduler_binding()->actor_ref()
        : diag::concurrency::NodeRef::cpu(
              cpu.descriptor->logical_id());
    while (thread != nullptr && thread->active_frame() != nullptr
        && !thread->current_wait().attached()
        && (cpu.dispatcher()->current().stop_requested()
            || thread->cancel_pending())) {
        thread->active_frame()->unwind(
            context, *cpu.dispatcher(), MYOS_STATUS_CANCELED);
    }
    operation::Wait* wait = thread != nullptr
        ? &thread->current_wait()
        : nullptr;
    const auto operation = wait != nullptr
        ? wait->observation_key()
        : diag::concurrency::ObservationKey{};
    diag::concurrency::ObservationBatch update{
        .phase = 1,
        .semantic_stamp = 1,
        .wait = operation
            ? diag::concurrency::WaitKind::OperationCompletion
            : diag::concurrency::WaitKind::SchedulerActivation,
        .driver = driver,
        .blocker = diag::concurrency::NodeRef::observation(operation),
        .site = diag::concurrency::SourceSite::current(),
        .update_progress = true,
        .update_watched = true,
        .watched = true};
    continuation.publish(update);
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
    /*luna change: consume a resumed terminal PageFault after serial Wait delivery, reason: Rearm remains attached while Done leaves the result for the shared trap policy*/
    if (thread != nullptr) {
        wait = &thread->current_wait();
        if (wait->page_fault().terminal()) {
            finish_thread_page_fault(
                *thread,
                wait->page_fault(),
                context,
                *cpu.dispatcher());
        }
    }
    // A canceled Endpoint frame cannot be popped while its leaf Wait still
    // owns the continuation. Once that relation has completed or canceled,
    // this same owning CPU performs the pending chain unwind.
    while (thread != nullptr && thread->active_frame() != nullptr
        && !thread->current_wait().attached()
        && (cpu.dispatcher()->current().stop_requested()
            || thread->cancel_pending())) {
        thread->active_frame()->unwind(
            context, *cpu.dispatcher(), MYOS_STATUS_CANCELED);
    }
    // A stop request deliberately waits for the subsystem continuation. Once
    // the relation is detached, give the dispatcher one final commit point.
    cpu.dispatcher()->on_trap_exit();
    KASSERT(cpu.current_execution() == execution);
    if (vproc != nullptr) {
        vproc->on_trap_exit(context);
    }
    continuation.finish(2);
}

} // namespace kernel::trap
