#include <sched/dispatcher.hpp>

#include <arch/context.hpp>
#include <arch/cpu.hpp>
#include <arch/interrupt.hpp>
#include <arch/ipi.hpp>
#include <arch/time.hpp>
#include <diag/console.hpp>
#include <core/debug.hpp>
#include <cpu/cpu_local.hpp>
#include <cpu/cpu_registry.hpp>
#include <cpu/cpu_runtime.hpp>
#include <sched/context.hpp>
#include <sched/domain.hpp>
#include <libk/checked_arithmetic.hpp>
#include <thread/thread.hpp>

namespace kernel::sched {

CpuDispatcher::CpuDispatcher(
    CpuLocal& cpu,
    CpuId id,
    Thread& idle,
    time::Clock& clock) noexcept
    : cpu_(&cpu), id_(id), idle_(&idle), clock_(&clock) {
    KASSERT(cpu_->dispatcher_ == nullptr);
    KASSERT(idle_->idle());
    KASSERT(idle_->state_ == Thread::State::Prepared);
    const auto quantum = clock_->duration_from_nanoseconds(4'000'000);
    KASSERT(quantum && !quantum->empty());
    quantum_ = *quantum;
    timer_available_ = arch::timer_available();
    ipi_available_ = arch::ipi_available();
    cpu_->dispatcher_ = this;
}

void CpuDispatcher::publish(Thread& thread) noexcept {
    KASSERT(!arch::interrupts_enabled());
    KASSERT(arch::current_cpu_owner() == cpu_);
    KASSERT(arch::trap_depth() == 0);
    const usize stack_top = thread.home_stack_top();
    KASSERT(stack_top != 0 && (stack_top & 0xfU) == 0);

    ExecutionBinding& execution = thread.execution_;
    execution.translation().activate(*cpu_);
    current_ = &thread;
    cpu_->current_thread_ = &thread;
    arch::publish_active_stack(cpu_->arch_state, stack_top);
}

[[noreturn]] void CpuDispatcher::enter_idle() noexcept {
    KASSERT(!arch::interrupts_enabled());
    KASSERT(current_ == nullptr);
    KASSERT(idle_->state_ == Thread::State::Prepared);
    idle_->state_ = Thread::State::Running;
    accounted_at_ = clock_->now();
    publish(*idle_);
    program_deadline(accounted_at_);
    record_dispatch(*idle_, *idle_, DispatchReason::Start, accounted_at_);
    arch::enter_context(idle_->context_);
}

void CpuDispatcher::on_context_enter() noexcept {
    KASSERT(!arch::interrupts_enabled());
    KASSERT(current_ != nullptr);
    KASSERT(cpu_->current_thread_ == current_);
    KASSERT(arch::active_stack(cpu_->arch_state)
        == current_->home_stack_top());
    KASSERT(cpu_->kernel_vspace() == current_->execution_.kernel_vspace());
    KASSERT(cpu_->vspace() == current_->execution_.vspace());
    KASSERT(cpu_->cspace() == current_->execution_.cspace());
    KASSERT(cpu_->active_translation_
        == &current_->execution_.translation().state());
    post_switch();
    if (ipi_available_) {
        arch::enable_ipi();
    }
    arch::enable_interrupts();
}

auto CpuDispatcher::make_ready(Binding& binding) noexcept -> bool {
    KASSERT(!arch::interrupts_enabled());
    if (binding.home_cpu() != id_ || binding.queued()) {
        return false;
    }
    Thread& thread = binding.thread();
    SchedulingContext& context = binding.context();
    if (thread.binding_ != &binding
        || context.binding() != &binding
        || !context.admitted()) {
        return false;
    }
    if (thread.state_ != Thread::State::Prepared
        && thread.state_ != Thread::State::Blocked
        && thread.state_ != Thread::State::Throttled) {
        return false;
    }
    const time::Instant now = clock_->now();
    if (binding.timer_queued()) {
        if (thread.state_ != Thread::State::Throttled) {
            return false;
        }
        if (!context.eligible(now)) {
            return true;
        }
        timers_.remove(binding);
    }
    enqueue_or_throttle(binding, now);
    if (thread.state_ == Thread::State::Throttled) {
        program_deadline(now);
        return true;
    }
    request_reschedule(DispatchReason::RemoteWake);
    return true;
}

auto CpuDispatcher::accept_wake(Binding& binding) noexcept
    -> WakeAcceptance {
    KASSERT(!arch::interrupts_enabled());
    if (binding.home_cpu() != id_) {
        return WakeAcceptance::Rejected;
    }
    Thread& thread = binding.thread();
    if (thread.state_ == Thread::State::Blocked) {
        if (!make_ready(binding)) {
            return WakeAcceptance::Rejected;
        }
        return thread.state_ == Thread::State::Ready
            ? WakeAcceptance::Readied
            : WakeAcceptance::Accepted;
    }
    if (thread.state_ == Thread::State::Running
        || thread.state_ == Thread::State::Ready
        || thread.state_ == Thread::State::Prepared) {
        binding.wake_credit_ = true;
        return WakeAcceptance::Accepted;
    }
    return thread.state_ == Thread::State::Throttled
        ? WakeAcceptance::Accepted
        : WakeAcceptance::Rejected;
}

auto CpuDispatcher::post_wake(Binding& binding) noexcept -> WakeResult {
    if (binding.home_cpu() != id_) {
        return libk::unexpected(WakeError::WrongCpu);
    }
    if (!ipi_available_) {
        return libk::unexpected(WakeError::Unavailable);
    }
    const WakeQueue::PostResult posted = wakes_.post(binding);
    KASSERT(posted.accepted);
    KASSERT(cpu_->descriptor != nullptr);
    for (usize attempt = 0; attempt < 8; ++attempt) {
        const auto transport = wakes_.claim_transport();
        if (!transport) {
            return libk::expected();
        }
        if (arch::send_ipi(cpu_->descriptor->hardware_id())) {
            return libk::expected();
        }
        wakes_.transport_failed(*transport);
    }
    KPANIC((diag::FatalEvent{
        .facility = diag::Facility::Scheduler,
        .id = diag::EventId{0x40000001},
        .arguments = {id_.raw},
        .argument_count = 1,
    }));
}

void CpuDispatcher::enqueue_or_throttle(
    Binding& binding,
    time::Instant now) noexcept {
    Thread& thread = binding.thread();
    SchedulingContext& context = binding.context();
    KASSERT(!binding.queued() && !binding.timer_queued());
    if (context.eligible(now)) {
        thread.state_ = Thread::State::Ready;
        policy_.enqueue(binding, context.urgency());
        return;
    }
    const auto deadline = context.next_refill();
    KASSERT(deadline && *deadline > now);
    thread.state_ = Thread::State::Throttled;
    timers_.insert(binding, *deadline);
}

void CpuDispatcher::process_timers(time::Instant now) noexcept {
    for (;;) {
        const auto deadline = timers_.deadline();
        if (!deadline || *deadline > now) {
            return;
        }
        Binding* const binding = timers_.front();
        KASSERT(binding != nullptr);
        timers_.remove(*binding);
        KASSERT(binding->thread().state_ == Thread::State::Throttled);
        enqueue_or_throttle(*binding, now);
    }
}

void CpuDispatcher::charge_to(time::Instant now) noexcept {
    const auto elapsed = now.elapsed_since(accounted_at_);
    KASSERT(elapsed);
    if (current_binding_ != nullptr && !elapsed->empty()) {
        current_binding_->context().charge(now, *elapsed);
    }
    if (!elapsed->empty()) {
        const auto total = libk::checked_add(
            pending_charge_.ticks(), elapsed->ticks());
        KASSERT(total);
        pending_charge_ = time::Duration::from_ticks(*total);
    }
    accounted_at_ = now;
}

void CpuDispatcher::yield() noexcept {
    KASSERT(!arch::interrupts_enabled());
    dispatch(DispatchReason::Yield, clock_->now());
}

void CpuDispatcher::block_current() noexcept {
    KASSERT(!arch::interrupts_enabled());
    KASSERT(current_ != nullptr && !current_->idle());
    KASSERT(current_binding_ != nullptr);
    if (current_binding_->wake_credit_) {
        current_binding_->wake_credit_ = false;
        return;
    }
    dispatch(DispatchReason::Block, clock_->now());
}

[[noreturn]] void CpuDispatcher::exit_current() noexcept {
    KASSERT(!arch::interrupts_enabled());
    KASSERT(current_ != nullptr && !current_->idle());
    dispatch(DispatchReason::Exit, clock_->now());
    KASSERT(false);
    __builtin_unreachable();
}

void CpuDispatcher::request_reschedule(DispatchReason reason) noexcept {
    if (!pending_ || reason == DispatchReason::Timer
        || reason == DispatchReason::Exit) {
        pending_ = reason;
    }
}

void CpuDispatcher::on_timer() noexcept {
    KASSERT(!arch::interrupts_enabled());
    const time::Instant now = clock_->now();
    charge_to(now);
    arch::mask_timer();
    request_reschedule(DispatchReason::Timer);
}

void CpuDispatcher::drain_remote_wakes() noexcept {
    KASSERT(!arch::interrupts_enabled());
    bool made_ready{};
    while (Binding* binding = wakes_.take()) {
        made_ready = accept_wake(*binding) == WakeAcceptance::Readied
            || made_ready;
        wakes_.complete(*binding);
    }
    if (made_ready) {
        request_reschedule(DispatchReason::RemoteWake);
    }
}

void CpuDispatcher::on_trap_exit() noexcept {
    KASSERT(!arch::interrupts_enabled());
    KASSERT(arch::trap_depth() == 0);
    if (!pending_ || preempt_depth_ != 0) {
        return;
    }
    const DispatchReason reason = *pending_;
    pending_.reset();
    if (reason == DispatchReason::Block) {
        block_current();
        return;
    }
    dispatch(reason, clock_->now());
}

void CpuDispatcher::disable_preemption() noexcept {
    KASSERT(!arch::interrupts_enabled());
    ++preempt_depth_;
    KASSERT(preempt_depth_ != 0);
}

void CpuDispatcher::enable_preemption() noexcept {
    KASSERT(!arch::interrupts_enabled());
    KASSERT(preempt_depth_ != 0);
    --preempt_depth_;
    if (preempt_depth_ == 0 && pending_ && arch::trap_depth() == 0) {
        on_trap_exit();
    }
}

void CpuDispatcher::dispatch(
    DispatchReason reason,
    time::Instant now) noexcept {
    KASSERT(!arch::interrupts_enabled());
    KASSERT(arch::trap_depth() == 0);
    KASSERT(current_ != nullptr);
    charge_to(now);
    process_timers(now);

    Thread* const outgoing = current_;
    Binding* const outgoing_binding = current_binding_;
    if (outgoing_binding != nullptr) {
        SchedulingContext& context = outgoing_binding->context();
        context.deactivate(id_);
        current_binding_ = nullptr;

        switch (reason) {
        case DispatchReason::Exit:
            outgoing->state_ = Thread::State::Exited;
            break;
        case DispatchReason::Block:
            outgoing->state_ = Thread::State::Blocked;
            break;
        case DispatchReason::Start:
        case DispatchReason::Yield:
        case DispatchReason::Timer:
        case DispatchReason::RemoteWake:
            if (context.eligible(now)) {
                outgoing->state_ = Thread::State::Ready;
                policy_.enqueue(*outgoing_binding, context.urgency());
            } else {
                enqueue_or_throttle(*outgoing_binding, now);
            }
            break;
        }
    }

    Binding* candidate = policy_.select().binding;
    if (candidate == nullptr && outgoing->idle()) {
        outgoing->state_ = Thread::State::Running;
        program_deadline(now);
        record_dispatch(*outgoing, *outgoing, reason, now);
        return;
    }
    commit(candidate, reason, now);

    if (reason == DispatchReason::Exit) {
        KASSERT(false);
        __builtin_unreachable();
    }
}

void CpuDispatcher::commit(
    Binding* candidate,
    DispatchReason reason,
    time::Instant now) noexcept {
    Thread* const outgoing = current_;
    Thread* incoming = idle_;
    Binding* incoming_binding{};

    if (candidate != nullptr) {
        SchedulingContext& context = candidate->context();
        Thread& target = candidate->thread();
        KASSERT(candidate->home_cpu() == id_);
        KASSERT(candidate->queued());
        KASSERT(target.state_ == Thread::State::Ready);
        KASSERT(target.binding_ == candidate);
        KASSERT(context.binding() == candidate);
        KASSERT(context.domain_ != nullptr);
        KASSERT(context.domain_->allows(id_));
        KASSERT(context.eligible(now));

        policy_.remove(*candidate, context.urgency());
        KASSERT(context.activate(id_));
        target.state_ = Thread::State::Running;
        incoming = &target;
        incoming_binding = candidate;
    } else {
        KASSERT(idle_->state_ == Thread::State::Prepared
            || idle_->state_ == Thread::State::Running);
        idle_->state_ = Thread::State::Running;
    }

    current_binding_ = incoming_binding;
    publish(*incoming);
    program_deadline(now);
    record_dispatch(*outgoing, *incoming, reason, now);

    if (incoming == outgoing) {
        return;
    }
    if (outgoing->idle()) {
        outgoing->state_ = Thread::State::Prepared;
    }

    KASSERT(handoff_outgoing_ == nullptr);
    handoff_outgoing_ = outgoing;
    arch::switch_context(outgoing->context_, incoming->context_);
    post_switch();
}

void CpuDispatcher::program_deadline(time::Instant now) noexcept {
    if (!timer_available_) {
        programmed_deadline_ = time::Instant::max();
        arch::mask_timer();
        return;
    }

    time::Instant deadline = time::Instant::max();
    if (current_binding_ != nullptr) {
        const time::Duration budget =
            current_binding_->context().available(now);
        const time::Duration slice = budget < quantum_ ? budget : quantum_;
        if (slice.empty()) {
            request_reschedule(DispatchReason::Timer);
            deadline = now;
        } else {
            const auto computed = now.checked_add(slice);
            KASSERT(computed);
            deadline = *computed;
        }
    }
    if (const auto timer_deadline = timers_.deadline();
        timer_deadline && *timer_deadline < deadline) {
        deadline = *timer_deadline;
    }
    const auto programmed = arch::program_timer(deadline);
    if (!programmed) {
        timer_available_ = false;
        programmed_deadline_ = time::Instant::max();
        arch::mask_timer();
    } else {
        programmed_deadline_ = deadline;
    }
}

void CpuDispatcher::record_dispatch(
    Thread& outgoing,
    Thread& incoming,
    DispatchReason reason,
    time::Instant now) noexcept {
    trace_.push(DispatchRecord{
        .cpu = id_,
        .outgoing = reinterpret_cast<usize>(&outgoing),
        .incoming = reinterpret_cast<usize>(&incoming),
        .context = reinterpret_cast<usize>(current_binding_ == nullptr
            ? nullptr
            : &current_binding_->context()),
        .reason = reason,
        .observed_at = now,
        .charged = pending_charge_,
        .deadline = programmed_deadline_,
        .ready_count = policy_.ready_count(),
        .timer_count = timers_.size(),
        .wake_count = wakes_.size(),
    });
    pending_charge_ = {};
}

void CpuDispatcher::dump_trace() const noexcept {
    for (const DispatchRecord& record : trace_.records()) {
        diag::console::print<
            "dispatch cpu={} old={} new={} sc={} reason={} charged={} "
            "deadline={} ready={} timers={} wakes={}\n">(
            record.cpu.raw,
            record.outgoing,
            record.incoming,
            record.context,
            static_cast<u64>(record.reason),
            record.charged.ticks(),
            record.deadline.ticks(),
            record.ready_count,
            record.timer_count,
            record.wake_count);
    }
}

void CpuDispatcher::post_switch() noexcept {
    KASSERT(!arch::interrupts_enabled());
    KASSERT(cpu_->current_thread_ == current_);
    KASSERT(current_ != nullptr);
    handoff_outgoing_ = nullptr;
}

void yield() noexcept {
    const arch::InterruptState interrupts = arch::disable_interrupts();
    CpuLocal& cpu = current_cpu();
    KASSERT(cpu.dispatcher() != nullptr);
    cpu.dispatcher()->yield();
    arch::restore_interrupts(interrupts);
}

void block() noexcept {
    const arch::InterruptState interrupts = arch::disable_interrupts();
    CpuLocal& cpu = current_cpu();
    KASSERT(cpu.dispatcher() != nullptr);
    cpu.dispatcher()->block_current();
    arch::restore_interrupts(interrupts);
}

auto wake(CpuRegistry& cpus, Binding& binding) noexcept
    -> CpuDispatcher::WakeResult {
    CpuRuntime* const target = cpus.runtime(binding.home_cpu());
    if (target == nullptr
        || target->local.descriptor->state() != CpuState::Online) {
        return libk::unexpected(CpuDispatcher::WakeError::Unavailable);
    }

    if (arch::current_cpu_owner() == &target->local) {
        const arch::InterruptState interrupts = arch::disable_interrupts();
        const CpuDispatcher::WakeAcceptance accepted =
            target->dispatcher().accept_wake(binding);
        arch::restore_interrupts(interrupts);
        if (accepted != CpuDispatcher::WakeAcceptance::Rejected) {
            return libk::expected();
        }
        return libk::unexpected(CpuDispatcher::WakeError::Unavailable);
    }
    return target->dispatcher().post_wake(binding);
}

[[noreturn]] void exit_current() noexcept {
    [[maybe_unused]] const arch::InterruptState interrupts =
        arch::disable_interrupts();
    CpuLocal& cpu = current_cpu();
    KASSERT(cpu.dispatcher() != nullptr);
    cpu.dispatcher()->exit_current();
}

} // namespace kernel::sched
