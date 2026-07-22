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
#include <sync/irq_lock_guard.hpp>
#include <libk/checked_arithmetic.hpp>
#include <libk/limits.hpp>
#include <thread/thread.hpp>
#include <execution/vproc.hpp>
#include <operation/wait.hpp>

namespace kernel::sched {

CpuDispatcher::CpuDispatcher(
    CpuLocal& cpu,
    CpuId id,
    Thread& idle,
    time::Clock& clock) noexcept
    : cpu_(&cpu), id_(id), idle_(&idle), clock_(&clock) {
    KASSERT(cpu_->dispatcher_ == nullptr);
    KASSERT(idle_->idle());
    KASSERT(idle_->execution_.state_ == ExecutionState::Prepared);
    const auto quantum = clock_->duration_from_nanoseconds(4'000'000);
    KASSERT(quantum && !quantum->empty());
    quantum_ = *quantum;
    timer_available_ = arch::timer_available();
    ipi_available_ = arch::ipi_available();
    cpu_->dispatcher_ = this;
}

auto CpuDispatcher::remaining_budget() const noexcept -> time::Duration {
    KASSERT(current_binding_ != nullptr);
    return current_binding_->context().available(clock_->now());
}

auto CpuDispatcher::current_urgency() const noexcept -> Urgency {
    KASSERT(current_binding_ != nullptr);
    return current_binding_->context().urgency();
}

auto CpuDispatcher::arm(
    Deadline& deadline,
    time::Instant when) noexcept -> bool {
    KASSERT(!arch::interrupts_enabled());
    KASSERT(arch::current_cpu_owner() == cpu_);
    if (!timer_available_ || deadline.armed() || !deadline.callback_) {
        return false;
    }
    deadlines_.insert(deadline, when);
    deadline.owner_ = this;
    program_deadline(clock_->now());
    return true;
}

void CpuDispatcher::disarm(Deadline& deadline) noexcept {
    KASSERT(!arch::interrupts_enabled());
    KASSERT(arch::current_cpu_owner() == cpu_);
    KASSERT(deadline.owner_ == this);
    deadlines_.remove(deadline);
    deadline.owner_ = nullptr;
    program_deadline(clock_->now());
}

void CpuDispatcher::publish(execution::Target target) noexcept {
    KASSERT(!arch::interrupts_enabled());
    KASSERT(arch::current_cpu_owner() == cpu_);
    KASSERT(target);
    Execution& execution = target.execution();
    const usize stack_top = target.stack_top();
    KASSERT(stack_top != 0 && (stack_top & 0xfU) == 0);

    ExecutionBinding& roots = target.effective_binding();
    roots.translation().activate(*cpu_);
    current_ = target;
    cpu_->current_execution_ = &execution;
    arch::publish_active_stack(cpu_->arch_state, stack_top);
}

[[noreturn]] void CpuDispatcher::enter_idle() noexcept {
    KASSERT(!arch::interrupts_enabled());
    KASSERT(!current_);
    KASSERT(idle_->execution_.state_ == ExecutionState::Prepared);
    idle_->execution_.state_ = ExecutionState::Running;
    accounted_at_ = clock_->now();
    publish(execution::Target{*idle_});
    program_deadline(accounted_at_);
    const execution::Target idle{*idle_};
    record_dispatch(idle, idle, DispatchReason::Start, accounted_at_);
    arch::enter_context(idle_->execution_.context());
}

void CpuDispatcher::on_context_enter() noexcept {
    KASSERT(!arch::interrupts_enabled());
    KASSERT(current_);
    KASSERT(cpu_->current_execution_ == &current_.execution());
    KASSERT(arch::active_stack(cpu_->arch_state)
        == current_.stack_top());
    ExecutionBinding& roots = current_.effective_binding();
    KASSERT(cpu_->kernel_vspace() == roots.kernel_vspace());
    KASSERT(cpu_->vspace() == roots.vspace());
    KASSERT(cpu_->cspace() == roots.cspace());
    KASSERT(cpu_->active_translation_
        == &roots.translation().state());
    post_switch();
    if (ipi_available_) {
        arch::enable_ipi();
    }
    arch::enable_interrupts();
}

void CpuDispatcher::refresh() noexcept {
    KASSERT(!arch::interrupts_enabled());
    // Endpoint frames change synchronously in the trap handler (depth 1) or
    // while an asynchronous terminal cause is committed by the common
    // trap-exit hook (depth 0). Both are owner-CPU, interrupts-off points
    // before the selected user frame is restored.
    KASSERT(arch::trap_depth() <= 1);
    KASSERT(current_ && cpu_->current_execution_ == &current_.execution());
    publish(current_);
}

auto CpuDispatcher::make_ready(Binding& binding) noexcept -> bool {
    KASSERT(!arch::interrupts_enabled());
    if (binding.home_cpu() != id_ || binding.queued()) {
        return false;
    }
    execution::Target target = binding.target();
    Execution& execution = target.execution();
    SchedulingContext& context = binding.context();
    if (execution.scheduler_binding_ != &binding
        || context.binding() != &binding
        || !context.admitted()) {
        return false;
    }
    if (execution.state_ != ExecutionState::Prepared
        && execution.state_ != ExecutionState::Blocked
        && execution.state_ != ExecutionState::Parked
        && execution.state_ != ExecutionState::Throttled) {
        return false;
    }
    if (!target.claim_home(*this)) {
        return false;
    }
    const time::Instant now = clock_->now();
    if (binding.timer_queued()) {
        if (execution.state_ != ExecutionState::Throttled) {
            return false;
        }
        if (!context.eligible(now)) {
            return true;
        }
        timers_.remove(binding);
    }
    enqueue_or_throttle(binding, now);
    if (execution.state_ == ExecutionState::Throttled) {
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
    Execution& execution = binding.execution();
    if (execution.state_ == ExecutionState::Blocked) {
        if (!make_ready(binding)) {
            return WakeAcceptance::Rejected;
        }
        return execution.state_ == ExecutionState::Ready
            ? WakeAcceptance::Readied
            : WakeAcceptance::Accepted;
    }
    if (execution.state_ == ExecutionState::Running
        || execution.state_ == ExecutionState::Ready
        || execution.state_ == ExecutionState::Prepared) {
        binding.wake_credit_ = true;
        return WakeAcceptance::Accepted;
    }
    return execution.state_ == ExecutionState::Throttled
        ? WakeAcceptance::Accepted
        : WakeAcceptance::Rejected;
}

auto CpuDispatcher::post_wake(Binding& binding) noexcept -> WakeResult {
    if (binding.home_cpu() != id_) {
        return libk::unexpected(WakeError::WrongCpu);
    }
    return post_remote(binding.wake_);
}

auto CpuDispatcher::post_start(Binding& binding) noexcept -> WakeResult {
    if (binding.home_cpu() != id_) {
        return libk::unexpected(WakeError::WrongCpu);
    }
    return post_remote(binding.start_);
}

auto CpuDispatcher::accept_activation(Vproc& vproc) noexcept -> bool {
    KASSERT(!arch::interrupts_enabled());
    Binding* binding{};
    bool wake_parked{};
    {
        kernel::sync::IrqLockGuard guard{vproc.state_lock_};
        binding = vproc.execution_.scheduler_binding_;
        if (binding == nullptr || binding->home_cpu() != id_
            || vproc.stop_requested_ || vproc.stopped_
            || vproc.upcall_state_ == Vproc::UpcallState::Unarmed) {
            return false;
        }
        if (vproc.upcall_state_ == Vproc::UpcallState::Active) {
            return true;
        }
        switch (vproc.execution_.state_) {
        case ExecutionState::Running:
            KASSERT(current_.vproc() == &vproc);
            binding->activation_credit_ = false;
            return true;
        case ExecutionState::Ready:
            binding->activation_credit_ = true;
            request_reschedule(DispatchReason::Activation);
            return true;
        case ExecutionState::Parked:
            binding->activation_credit_ = true;
            wake_parked = true;
            break;
        case ExecutionState::Prepared:
        case ExecutionState::Blocked:
        case ExecutionState::Throttled:
            binding->activation_credit_ = true;
            return true;
        case ExecutionState::Exited:
            return false;
        }
    }
    return wake_parked && make_ready(*binding);
}

auto CpuDispatcher::post_activation(Vproc& vproc) noexcept -> WakeResult {
    return post_remote(vproc.activation_);
}

auto CpuDispatcher::request_activation(
    CpuRegistry& cpus,
    Vproc& vproc) noexcept -> WakeResult {
    CpuId home{};
    {
        kernel::sync::IrqLockGuard guard{vproc.state_lock_};
        Binding* const binding = vproc.execution_.scheduler_binding_;
        if (vproc.upcall_state_ != Vproc::UpcallState::Armed) {
            return libk::expected();
        }
        if (binding == nullptr || vproc.stop_requested_ || vproc.stopped_) {
            return libk::unexpected(WakeError::Unavailable);
        }
        KASSERT(vproc.activation_publishers_
            != libk::numeric_limits<usize>::max());
        ++vproc.activation_publishers_;
        home = binding->home_cpu();
    }
    CpuRuntime* const target = cpus.runtime(home);
    if (target == nullptr
        || target->local.descriptor->state() != CpuState::Online) {
        vproc.activation_publisher_done();
        return libk::unexpected(WakeError::Unavailable);
    }
    if (arch::current_cpu_owner() == &target->local) {
        kernel::sync::IrqToken irq{};
        const bool accepted = target->dispatcher().accept_activation(vproc);
        vproc.activation_publisher_done();
        return accepted
            ? WakeResult{libk::expected()}
            : WakeResult{libk::unexpected(WakeError::Unavailable)};
    }
    {
        kernel::sync::IrqLockGuard guard{vproc.state_lock_};
        KASSERT(vproc.activation_publishers_ != 0);
        --vproc.activation_publishers_;
        if (vproc.activation_post_ != Vproc::ActivationPost::Idle) {
            // A publication concurrent with an already consumed request must
            // receive another home-CPU admission pass. Publications that race
            // an unconsumed request may be folded into that pass, but treating
            // all coalescing as dirty keeps the ordering proof local.
            vproc.activation_dirty_ = true;
            return libk::expected();
        }
        KASSERT(!vproc.activation_dirty_);
        vproc.activation_post_ = Vproc::ActivationPost::Posting;
    }
    for (;;) {
        auto posted = target->dispatcher().post_activation(vproc);
        const bool repost = vproc.activation_request_posted(
            static_cast<bool>(posted));
        if (!posted || !repost) {
            return posted;
        }
    }
}

auto CpuDispatcher::post_remote(RemoteRequest& request) noexcept -> WakeResult {
    if (!ipi_available_) {
        return libk::unexpected(WakeError::Unavailable);
    }
    remote_.post(request);
    return kick_remote();
}

auto CpuDispatcher::kick_remote() noexcept -> WakeResult {
    if (!ipi_available_) {
        return libk::unexpected(WakeError::Unavailable);
    }
    KASSERT(cpu_->descriptor != nullptr);
    for (usize attempt = 0; attempt < 8; ++attempt) {
        const auto transport = remote_.claim_transport();
        if (!transport) {
            return libk::expected();
        }
        if (arch::send_ipi(cpu_->descriptor->hardware_id())) {
            return libk::expected();
        }
        remote_.transport_failed(*transport);
    }
    KPANIC((diag::FatalEvent{
        .facility = diag::Facility::Scheduler,
        .id = diag::EventId{0x40000001},
        .arguments = {id_.raw},
        .argument_count = 1,
    }));
}

void CpuDispatcher::request_stop(Thread& thread) noexcept {
    request_stop(execution::Target{thread});
}

void CpuDispatcher::request_stop(Vproc& vproc) noexcept {
    request_stop(execution::Target{vproc});
}

void CpuDispatcher::request_stop(execution::Target target) noexcept {
    if (arch::current_cpu_owner() == cpu_) {
        kernel::sync::IrqToken irq{};
        // A target may finish its ordinary exit after the stop owner publishes
        // the terminal transaction but before this owner-CPU call is entered.
        // The queued Stop has already been completed by finish_stop() then.
        if (target.stopped()) {
            return;
        }
        if (stop(target) == StopDisposition::Finalize) {
            finish_exit(target);
        }
        return;
    }

    if (!ipi_available_) {
        KPANIC((diag::FatalEvent{
            .facility = diag::Facility::Scheduler,
            .id = diag::EventId{0x40000002},
            .arguments = {id_.raw},
            .argument_count = 1,
        }));
    }

    bool queued{};
    bool owned{};
    if (Vproc* const vproc = target.vproc()) {
        kernel::sync::IrqLockGuard guard{vproc->state_lock_};
        Binding* const binding = vproc->execution_.scheduler_binding_;
        owned = vproc->execution_.home_ == this;
        if (binding != nullptr) {
            KASSERT(owned && binding->target() == target);
            remote_.post(binding->stop_);
            queued = true;
        } else if (vproc->stopped_) {
            KASSERT(vproc->execution_.home_ == nullptr
                && vproc->execution_.state_ == ExecutionState::Exited);
        } else {
            KASSERT(owned && vproc->execution_.state_ == ExecutionState::Exited
                && vproc->stop_requested_ && vproc->stop_dispatched_);
        }
    } else {
        Thread* const thread = target.thread();
        KASSERT(thread != nullptr);
        kernel::sync::IrqLockGuard guard{thread->stop_lock_};
        Binding* const binding = thread->execution_.scheduler_binding_;
        owned = thread->execution_.home_ == this;
        if (binding != nullptr) {
            KASSERT(owned && binding->target() == target);
            remote_.post(binding->stop_);
            queued = true;
        } else if (thread->stopped_) {
            KASSERT(thread->execution_.home_ == nullptr
                && thread->execution_.state_ == ExecutionState::Exited);
        } else {
            KASSERT(owned && thread->execution_.state_ == ExecutionState::Exited
                && thread->stop_requested_);
        }
    }
    if (queued && !kick_remote()) {
        KPANIC((diag::FatalEvent{
            .facility = diag::Facility::Scheduler,
            .id = diag::EventId{0x40000003},
            .arguments = {id_.raw},
            .argument_count = 1,
        }));
    }
}

void CpuDispatcher::drain_remote() noexcept {
    KASSERT(!arch::interrupts_enabled());
    bool made_ready{};
    while (RemoteRequest* request = remote_.take()) {
        switch (request->kind()) {
        case RemoteKind::Start: {
            auto& binding = *static_cast<Binding*>(request->owner());
            made_ready = make_ready(binding) || made_ready;
            break;
        }
        case RemoteKind::Wake: {
            auto& binding = *static_cast<Binding*>(request->owner());
            made_ready = accept_wake(binding) == WakeAcceptance::Readied
                || made_ready;
            break;
        }
        case RemoteKind::Activation: {
            auto& vproc = *static_cast<Vproc*>(request->owner());
            remote_.complete(*request);
            do {
                static_cast<void>(accept_activation(vproc));
            } while (vproc.activation_request_consumed());
            continue;
        }
        case RemoteKind::Stop: {
            const execution::Target target =
                static_cast<Binding*>(request->owner())->target();
            const StopDisposition disposition = stop(target);
            // stop() may make the Binding terminal, but the consumed request
            // still owns its embedded storage until complete().  Release the
            // queue protocol before final relation teardown destroys Binding.
            remote_.complete(*request);
            if (disposition == StopDisposition::Finalize) {
                finish_exit(target);
            }
            continue;
        }
        }
        remote_.complete(*request);
    }
    if (made_ready) {
        request_reschedule(DispatchReason::RemoteWake);
    }
}

void CpuDispatcher::enqueue_or_throttle(
    Binding& binding,
    time::Instant now) noexcept {
    Execution& execution = binding.execution();
    SchedulingContext& context = binding.context();
    KASSERT(!binding.queued() && !binding.timer_queued());
    if (context.eligible(now)) {
        execution.state_ = ExecutionState::Ready;
        policy_.enqueue(binding, context.urgency());
        return;
    }
    const auto deadline = context.next_refill();
    KASSERT(deadline && *deadline > now);
    execution.state_ = ExecutionState::Throttled;
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
        KASSERT(binding->execution().state_
            == ExecutionState::Throttled);
        enqueue_or_throttle(*binding, now);
    }
}

void CpuDispatcher::process_deadlines(time::Instant now) noexcept {
    for (;;) {
        const auto when = deadlines_.deadline();
        if (!when || *when > now) {
            return;
        }
        Deadline* const deadline = deadlines_.front();
        KASSERT(deadline != nullptr && deadline->owner_ == this);
        deadlines_.remove(*deadline);
        deadline->owner_ = nullptr;
        const Deadline::Callback callback = deadline->callback_;
        KASSERT(callback);
        callback();
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
    KASSERT(current_ && !current_.idle());
    KASSERT(current_binding_ != nullptr);
    if (current_binding_->wake_credit_) {
        current_binding_->wake_credit_ = false;
        return;
    }
    dispatch(DispatchReason::Block, clock_->now());
}

void CpuDispatcher::park_current() noexcept {
    KASSERT(!arch::interrupts_enabled());
    dispatch(DispatchReason::Park, clock_->now());
}

[[noreturn]] void CpuDispatcher::exit_current() noexcept {
    KASSERT(!arch::interrupts_enabled());
    KASSERT(current_ && !current_.idle());
    dispatch(DispatchReason::Exit, clock_->now());
    KASSERT(false);
    __builtin_unreachable();
}

void CpuDispatcher::request_reschedule(DispatchReason reason) noexcept {
    if (!pending_ || reason == DispatchReason::Timer
        || reason == DispatchReason::Exit
        || reason == DispatchReason::Stop) {
        pending_ = reason;
    }
}

void CpuDispatcher::on_timer() noexcept {
    KASSERT(!arch::interrupts_enabled());
    const time::Instant now = clock_->now();
    charge_to(now);
    arch::mask_timer();
    process_deadlines(now);
    request_reschedule(DispatchReason::Timer);
}

void CpuDispatcher::on_trap_exit() noexcept {
    KASSERT(!arch::interrupts_enabled());
    KASSERT(arch::trap_depth() == 0);
    if (current_.stop_ready()) {
        request_reschedule(DispatchReason::Stop);
    }
    if (!pending_ || preempt_depth_ != 0) {
        return;
    }
    const DispatchReason reason = *pending_;
    pending_.reset();
    if (reason == DispatchReason::Block) {
        block_current();
        return;
    }
    if (reason == DispatchReason::Park) {
        park_current();
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
    KASSERT(current_);
    charge_to(now);
    process_timers(now);

    const execution::Target outgoing = current_;
    Binding* const outgoing_binding = current_binding_;
    bool parked{};
    if (reason == DispatchReason::Park) {
        Vproc* const vproc = outgoing.vproc();
        KASSERT(vproc != nullptr && outgoing_binding != nullptr);
        kernel::sync::IrqLockGuard guard{vproc->state_lock_};
        if (!vproc->park_requested_
            || vproc->park_sequence_ != vproc->pending_sequence_
            || vproc->ready_mask_ != 0 || vproc->ingress_mask_ != 0
            || vproc->notification_mask_ != 0
            || vproc->stop_requested_
            || vproc->execution_.state_ != ExecutionState::Running) {
            vproc->park_requested_ = false;
            return;
        }
        SchedulingContext& context = outgoing_binding->context();
        context.deactivate(id_);
        current_binding_ = nullptr;
        vproc->park_requested_ = false;
        vproc->execution_.state_ = ExecutionState::Parked;
        parked = true;
    } else if (outgoing_binding != nullptr) {
        SchedulingContext& context = outgoing_binding->context();
        context.deactivate(id_);
        current_binding_ = nullptr;

        switch (reason) {
        case DispatchReason::Exit:
        case DispatchReason::Stop:
            outgoing.execution().state_ = ExecutionState::Exited;
            break;
        case DispatchReason::Block:
            outgoing.execution().state_ = ExecutionState::Blocked;
            break;
        case DispatchReason::Park:
            KASSERT(false);
            break;
        case DispatchReason::Start:
        case DispatchReason::Yield:
        case DispatchReason::Timer:
        case DispatchReason::RemoteWake:
        case DispatchReason::Activation:
            if (context.eligible(now)) {
                outgoing.execution().state_ = ExecutionState::Ready;
                policy_.enqueue(*outgoing_binding, context.urgency());
            } else {
                enqueue_or_throttle(*outgoing_binding, now);
            }
            break;
        }
    }
    KASSERT(reason != DispatchReason::Park || parked);

    Binding* const candidate = policy_.select().binding;
    if (candidate == nullptr && outgoing.idle()) {
        outgoing.execution().state_ = ExecutionState::Running;
        program_deadline(now);
        record_dispatch(outgoing, outgoing, reason, now);
        return;
    }
    commit(candidate, reason, now);

    if (reason == DispatchReason::Exit || reason == DispatchReason::Stop) {
        KASSERT(false);
        __builtin_unreachable();
    }
}

void CpuDispatcher::commit(
    Binding* candidate,
    DispatchReason reason,
    time::Instant now) noexcept {
    const execution::Target outgoing = current_;
    execution::Target incoming{*idle_};
    Binding* incoming_binding{};

    if (candidate != nullptr) {
        SchedulingContext& context = candidate->context();
        execution::Target target = candidate->target();
        Execution& execution = target.execution();
        KASSERT(candidate->home_cpu() == id_);
        KASSERT(candidate->queued());
        KASSERT(execution.state_ == ExecutionState::Ready);
        KASSERT(execution.scheduler_binding_ == candidate);
        KASSERT(context.binding() == candidate);
        KASSERT(context.domain_ != nullptr);
        KASSERT(context.domain_->allows(id_));
        KASSERT(context.eligible(now));

        candidate->activation_credit_ = false;
        policy_.remove(*candidate, context.urgency());
        KASSERT(context.activate(id_));
        execution.state_ = ExecutionState::Running;
        incoming = target;
        incoming_binding = candidate;
    } else {
        KASSERT(idle_->execution_.state_ == ExecutionState::Prepared
            || idle_->execution_.state_ == ExecutionState::Running);
        idle_->execution_.state_ = ExecutionState::Running;
    }

    current_binding_ = incoming_binding;
    publish(incoming);
    program_deadline(now);
    record_dispatch(outgoing, incoming, reason, now);

    if (incoming == outgoing) {
        return;
    }
    if (outgoing.idle()) {
        outgoing.execution().state_ = ExecutionState::Prepared;
    }

    KASSERT(!handoff_outgoing_);
    handoff_outgoing_ = outgoing;
    if constexpr (kernel::sync::lock_verify) {
        kernel::sync::assert_no_locks();
    }
    arch::switch_context(
        outgoing.execution().context(), incoming.execution().context());
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
    if (const auto call_deadline = deadlines_.deadline();
        call_deadline && *call_deadline < deadline) {
        deadline = *call_deadline;
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
    execution::Target outgoing,
    execution::Target incoming,
    DispatchReason reason,
    time::Instant now) noexcept {
    trace_.push(DispatchRecord{
        .cpu = id_,
        .outgoing = outgoing.identity(),
        .incoming = incoming.identity(),
        .context = reinterpret_cast<usize>(current_binding_ == nullptr
            ? nullptr
            : &current_binding_->context()),
        .reason = reason,
        .observed_at = now,
        .charged = pending_charge_,
        .deadline = programmed_deadline_,
        .ready_count = policy_.ready_count(),
        .timer_count = timers_.size(),
            .remote_count = remote_.size(),
    });
    pending_charge_ = {};
}

void CpuDispatcher::dump_trace() const noexcept {
    for (const DispatchRecord& record : trace_.records()) {
        diag::console::print<
            "dispatch cpu={} old={} new={} sc={} reason={} charged={} "
            "deadline={} ready={} timers={} remote={}\n">(
            record.cpu.raw,
            record.outgoing,
            record.incoming,
            record.context,
            static_cast<u64>(record.reason),
            record.charged.ticks(),
            record.deadline.ticks(),
            record.ready_count,
            record.timer_count,
            record.remote_count);
    }
}

void CpuDispatcher::post_switch() noexcept {
    KASSERT(!arch::interrupts_enabled());
    KASSERT(cpu_->current_execution_ == &current_.execution());
    KASSERT(current_);
    const execution::Target outgoing = handoff_outgoing_;
    handoff_outgoing_ = {};
    if (outgoing
        && outgoing.execution().state_ == ExecutionState::Exited) {
        finish_exit(outgoing);
    }
}

auto CpuDispatcher::stop(execution::Target target) noexcept
    -> StopDisposition {
    KASSERT(!arch::interrupts_enabled());
    KASSERT(target.owned_by(*this));
    Execution& execution = target.execution();

    if (operation::Wait* const wait = target.wait();
        wait != nullptr && wait->attached()) {
        if (!wait->cancel()) {
            return StopDisposition::Deferred;
        }
        if (execution.state_ == ExecutionState::Blocked) {
            Binding* const binding = execution.scheduler_binding_;
            KASSERT(binding != nullptr);
            static_cast<void>(accept_wake(*binding));
            return StopDisposition::Deferred;
        }
    }

    if (Vproc* const vproc = target.vproc()) {
        const RemoteCancel canceled = remote_.cancel(vproc->activation_);
        // stop_ready() is derived from the Vproc-owned post state. Reaching
        // the home dispatcher therefore implies that queue ownership is gone.
        KASSERT(canceled == RemoteCancel::NotPending);
    }

    if (execution.state_ == ExecutionState::Running) {
        KASSERT(current_ == target);
        request_reschedule(DispatchReason::Stop);
        return StopDisposition::Deferred;
    }
    if ((target.vproc() != nullptr && target.stop_deferred())
        || (execution.state_ == ExecutionState::Blocked
            && target.stop_deferred())) {
        // The operation owns the continuation and may already be completing
        // on another CPU. Let its retained wake make the frame runnable; trap
        // exit consumes the result before the pending stop is committed.
        return StopDisposition::Deferred;
    }

    Binding* const binding = execution.scheduler_binding_;
    if (binding != nullptr) {
        if (binding->queued()) {
            policy_.remove(*binding, binding->context().urgency());
        }
        if (binding->timer_queued()) {
            timers_.remove(*binding);
        }
        binding->activation_credit_ = false;
        static_cast<void>(remote_.cancel(binding->start_));
        static_cast<void>(remote_.cancel(binding->wake_));
        static_cast<void>(remote_.cancel(binding->stop_));
    }
    execution.state_ = ExecutionState::Exited;
    return StopDisposition::Finalize;
}

void CpuDispatcher::finish_exit(execution::Target target) noexcept {
    KASSERT(!arch::interrupts_enabled());
    Execution& execution = target.execution();
    KASSERT(execution.state_ == ExecutionState::Exited);
    KASSERT(current_ != target);
    KASSERT(!target.stop_deferred());
    if (execution.scheduler_binding_ != nullptr) {
        Binding& binding = *execution.scheduler_binding_;
        KASSERT(!binding.queued() && !binding.timer_queued());
        static_cast<void>(remote_.cancel(binding.start_));
        static_cast<void>(remote_.cancel(binding.wake_));
        static_cast<void>(remote_.cancel(binding.stop_));
        if (Vproc* const vproc = target.vproc()) {
            const RemoteCancel canceled = remote_.cancel(vproc->activation_);
            KASSERT(canceled == RemoteCancel::NotPending);
        }
        KASSERT(binding.context().unbind(this));
    }
    target.finish_stop();
}

void yield() noexcept {
    kernel::sync::IrqToken irq{};
    CpuLocal& cpu = current_cpu();
    KASSERT(cpu.dispatcher() != nullptr);
    cpu.dispatcher()->yield();
}

void block() noexcept {
    kernel::sync::IrqToken irq{};
    CpuLocal& cpu = current_cpu();
    KASSERT(cpu.dispatcher() != nullptr);
    cpu.dispatcher()->block_current();
}

auto wake(CpuRegistry& cpus, Binding& binding) noexcept
    -> CpuDispatcher::WakeResult {
    CpuRuntime* const target = cpus.runtime(binding.home_cpu());
    if (target == nullptr
        || target->local.descriptor->state() != CpuState::Online) {
        return libk::unexpected(CpuDispatcher::WakeError::Unavailable);
    }

    if (arch::current_cpu_owner() == &target->local) {
        kernel::sync::IrqToken irq{};
        const CpuDispatcher::WakeAcceptance accepted =
            target->dispatcher().accept_wake(binding);
        if (accepted != CpuDispatcher::WakeAcceptance::Rejected) {
            return libk::expected();
        }
        return libk::unexpected(CpuDispatcher::WakeError::Unavailable);
    }
    return target->dispatcher().post_wake(binding);
}

auto start(CpuRegistry& cpus, Binding& binding) noexcept
    -> CpuDispatcher::WakeResult {
    CpuRuntime* const target = cpus.runtime(binding.home_cpu());
    if (target == nullptr
        || target->local.descriptor->state() != CpuState::Online) {
        return libk::unexpected(CpuDispatcher::WakeError::Unavailable);
    }
    if (arch::current_cpu_owner() == &target->local) {
        kernel::sync::IrqToken irq{};
        const bool accepted = target->dispatcher().make_ready(binding);
        return accepted
            ? CpuDispatcher::WakeResult{libk::expected()}
            : CpuDispatcher::WakeResult{
                  libk::unexpected(CpuDispatcher::WakeError::Unavailable)};
    }
    return target->dispatcher().post_start(binding);
}

auto activate(CpuRegistry& cpus, Vproc& vproc) noexcept
    -> CpuDispatcher::WakeResult {
    return CpuDispatcher::request_activation(cpus, vproc);
}

[[noreturn]] void exit_current() noexcept {
    [[maybe_unused]] const arch::InterruptState interrupts =
        arch::disable_interrupts();
    CpuLocal& cpu = current_cpu();
    KASSERT(cpu.dispatcher() != nullptr);
    cpu.dispatcher()->exit_current();
}

} // namespace kernel::sched
