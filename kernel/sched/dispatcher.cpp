#include <cpu/ipi.hpp>
#include <expected>
#include <bit>
#include <utility>
#include <sched/dispatcher.hpp>
#include <trace.hpp>

#include <cpu.hpp>
#include <console.hpp>
#include <panic.hpp>
#include <cpu/cpu.hpp>
#include <sched/sc.hpp>
#include <sched/domain.hpp>
#include <sync.hpp>
#include <libk/checked_arithmetic.hpp>
#include <limits>
#include <task/thread.hpp>
#include <wait.hpp>

namespace sched {

Dispatcher::Dispatcher(
    Cpu& cpu,
    Thread& idle,
    time::Clock& clock) noexcept
    : cpu_(cpu), idle_(&idle), clock_(&clock)
{
    libk_assert(idle_->idle());
    libk_assert(idle_->state_ == Thread::State::Prepared);
    const auto quantum = clock_->duration_from_nanoseconds(4'000'000);
    libk_assert(quantum && !quantum->empty());
    quantum_ = *quantum;
    timer_available_ = arch::timer_available();
    ipi_available_ = arch::ipi_available();
}

auto Dispatcher::id() const noexcept -> CpuId { return cpu_.id; }

void Dispatcher::enqueue(Sc& sc) noexcept {
    libk_assert(!sc.queued());
    const auto level = sc.urgency().value();
    ready_[level].push_back(sc);
    ready_mask_ |= u32{1} << level;
    ++ready_count_;
}

void Dispatcher::remove(Sc& sc) noexcept {
    libk_assert(sc.queued());
    const auto level = sc.urgency().value();
    ready_[level].erase(sc);
    --ready_count_;
    if (ready_[level].empty()) ready_mask_ &= ~(u32{1} << level);
}

Sc* Dispatcher::select() noexcept {
    return ready_mask_ ? &ready_[31 - std::countl_zero(ready_mask_)].front() : nullptr;
}

auto Dispatcher::current() const noexcept -> Thread* {
    return cpu_.current;
}

auto Dispatcher::remaining_budget() const noexcept -> time::Duration {
    libk_assert(current_sc_ != nullptr);
    return current_sc_->available(clock_->now());
}

auto Dispatcher::current_urgency() const noexcept -> Urgency {
    libk_assert(current_sc_ != nullptr);
    return current_sc_->urgency();
}

auto Dispatcher::arm(
    Deadline& deadline,
    time::Instant when) noexcept -> bool {
    libk_assert(!arch::interrupts_enabled());
    libk_assert((arch::local() ? arch::local()->owner : nullptr) == &cpu_);
    if (!timer_available_ || deadline.armed() || !deadline.callback_) {
        return false;
    }
    deadline.when_ = when;
    deadlines_.insert(deadline);
    deadline.owner_ = this;
    program_deadline(clock_->now());
    return true;
}

void Dispatcher::disarm(Deadline& deadline) noexcept {
    libk_assert(!arch::interrupts_enabled());
    libk_assert((arch::local() ? arch::local()->owner : nullptr) == &cpu_);
    libk_assert(deadline.owner_ == this);
    deadlines_.erase(deadline);
    deadline.owner_ = nullptr;
    program_deadline(clock_->now());
}

void Dispatcher::publish(Thread* target) noexcept {
    libk_assert(!arch::interrupts_enabled());
    libk_assert((arch::local() ? arch::local()->owner : nullptr) == &cpu_);
    libk_assert(target);
    const usize stack_top = target->current_stack_top();
    libk_assert(stack_top != 0 && (stack_top & 0xfU) == 0);

    Env& roots = target->env();
    roots.root().activate(cpu_);
    cpu_.current = target;
    cpu_.entry.stack = stack_top;
}

[[noreturn]] void Dispatcher::enter_idle() noexcept {
    libk_assert(!arch::interrupts_enabled());
    libk_assert(!cpu_.current);
    libk_assert(idle_->state_ == Thread::State::Prepared);
    idle_->set_state(Thread::State::Running);
    accounted_at_ = clock_->now();
    publish(idle_);
    program_deadline(accounted_at_);
    Thread* idle = idle_;
    record_dispatch(idle, idle, DispatchReason::Start, accounted_at_);
    arch::enter_ctx(idle_->ctx());
}

void Dispatcher::on_context_enter() noexcept {
    libk_assert(!arch::interrupts_enabled());
    libk_assert(cpu_.current);
    libk_assert(cpu_.entry.stack
        == cpu_.current->current_stack_top());
    Env& roots = cpu_.current->env();
    libk_assert(cpu_.tlb
        == &roots.root().state());
    post_switch();
    if (ipi_available_) {
        arch::enable_ipi();
    }
    arch::enable_interrupts();
}

void Dispatcher::refresh() noexcept {
    libk_assert(!arch::interrupts_enabled());
    // Endpoint frames change synchronously in the trap handler (depth 1) or
    // while an asynchronous terminal cause is committed by the common
    // trap-exit hook (depth 0). Both are owner-CPU, interrupts-off points
    // before the selected user frame is restored.
    libk_assert(arch::local()->depth <= 1);
    libk_assert(cpu_.current);
    publish(cpu_.current);
}

auto Dispatcher::make_ready(Sc& sc) noexcept -> bool {
    libk_assert(!arch::interrupts_enabled());
    if (sc.home_cpu() != cpu_.id || sc.queued()) {
        return false;
    }
    Thread* target = &sc.thread();
    Thread& exec = *target;
    Sc& context = sc;
    if (exec.sc_ != &sc
        || !context.admitted()) {
        return false;
    }
    if (exec.state_ != Thread::State::Prepared
        && exec.state_ != Thread::State::Blocked
        && exec.state_ != Thread::State::Throttled) {
        return false;
    }
    if (!target->claim_home(*this)) {
        return false;
    }
    const time::Instant now = clock_->now();
    if (sc.timer_queued()) {
        if (exec.state_ != Thread::State::Throttled) {
            return false;
        }
        if (!context.eligible(now)) {
            return true;
        }
        timers_.erase(sc);
    }
    enqueue_or_throttle(sc, now);

    if (exec.state_ == Thread::State::Throttled) {
        program_deadline(now);
        return true;
    }
    request_reschedule(DispatchReason::RemoteWake);
    return true;
}

auto Dispatcher::accept_wake(
    Sc& sc) noexcept
    -> WakeAcceptance {
    libk_assert(!arch::interrupts_enabled());
    if (sc.home_cpu() != cpu_.id) {
        return WakeAcceptance::Rejected;
    }
    Thread& exec = sc.thread();
    if (exec.state_ == Thread::State::Blocked) {
        if (!make_ready(sc)) {
            return WakeAcceptance::Rejected;
        }

        return exec.state_ == Thread::State::Ready
            ? WakeAcceptance::Readied
            : WakeAcceptance::Accepted;
    }
    if (exec.state_ == Thread::State::Running
        || exec.state_ == Thread::State::Ready
        || exec.state_ == Thread::State::Prepared) {
        sc.wake_credit_ = true;

        return WakeAcceptance::Accepted;
    }
    if (exec.state_ == Thread::State::Throttled) {
        // A wake accepted while the continuation is budget-throttled must
        // survive the refill and the resumed stack until block_current().
        // The Blocked arm above deliberately does not set this bit: its
        // make_ready() transition already consumes that wake.
        sc.wake_credit_ = true;

        return WakeAcceptance::Accepted;
    }
    return WakeAcceptance::Rejected;
}

auto Dispatcher::post_wake(
    Sc& sc) noexcept -> WakeResult {
    if (sc.home_cpu() != cpu_.id) {
        return std::unexpected(WakeError::WrongCpu);
    }
    return post_remote(sc, Wake);
}

auto Dispatcher::post_start(Sc& sc) noexcept -> WakeResult {
    if (sc.home_cpu() != cpu_.id) {
        return std::unexpected(WakeError::WrongCpu);
    }
    return post_remote(sc, Start);
}

void Dispatcher::post(Sc& sc, u8 actions) noexcept {
    sync::Lock guard{mail_lock_};
    sc.actions_ |= actions;
    if (!sc.mailed_.load<libk::MemoryOrder::Relaxed>()) {
        sc.mailed_.store<libk::MemoryOrder::Release>(true);
        mail_.push_back(sc);
    }
    trace::emit(trace::Event::Post, reinterpret_cast<u64>(&sc), actions);
}

void Dispatcher::complete(Sc& sc) noexcept {
    sync::Lock guard{mail_lock_};
    libk_assert(sc.mailed_.load<libk::MemoryOrder::Relaxed>() && !sc.mail_hook_.is_linked());
    if (sc.actions_) mail_.push_back(sc);
    else sc.mailed_.store<libk::MemoryOrder::Release>(false);
}

auto Dispatcher::post_remote(Sc& sc, u8 actions) noexcept -> WakeResult {
    if (!ipi_available_) return std::unexpected(WakeError::Unavailable);
    {
        // Serialize publication with external unbind. The binding keeps both
        // Sc and Thread resident until the home consumer completes its mail.
        sync::Lock guard{sc.authority_lock_};
        if (!sc.owner_) return std::unexpected(WakeError::Unavailable);
        sync::Lock target_guard{sc.thread().lock_};
        if (sc.thread().sc_ != &sc) return std::unexpected(WakeError::Unavailable);
        post(sc, actions);
    }
    return kick_remote();
}

auto Dispatcher::kick_remote() noexcept -> WakeResult {
    if (!ipi_available_) {
        return std::unexpected(WakeError::Unavailable);
    }
    for (usize attempt = 0; attempt < 8; ++attempt) {
        {
            sync::Lock guard{mail_lock_};
            if (mail_.empty()) return {};
        }
        if (send_ipi(cpu_.hw)) {
            trace::emit(trace::Event::Ipi, cpu_.id.raw, cpu_.hw.raw);
            return {};
        }
        trace::emit(trace::Event::KickFail, cpu_.id.raw, cpu_.hw.raw, attempt);
    }
    panic("IPI delivery failed");
}

void Dispatcher::request_stop(Thread& entity) noexcept {
    auto* target = &entity;
    if ((arch::local() ? arch::local()->owner : nullptr) == &cpu_) {
        sync::Irq irq{};
        // A target may finish its ordinary exit after the stop owner publishes
        // the terminal transaction but before this owner-CPU call is entered.
        // The queued Stop has already been completed by finish_stop() then.
        if (target->stopped()) {
            return;
        }
        if (stop(target) == StopDisposition::Finalize) {
            finish_exit(target, DispatchReason::Stop);
        }
        return;
    }

    if (!ipi_available_) {
        panic("remote stop requires IPI");
    }

    bool queued{};
    {
        sync::Lock guard{entity.lock_};
        Sc* const sc = entity.sc_;
        const bool owned = entity.home_ == this;
        if (sc != nullptr) {
            libk_assert(owned && &sc->thread() == &entity);
            post(*sc, Stop);
            queued = true;
        } else if (entity.stopped_) {
            libk_assert(entity.home_ == nullptr && entity.state_ == Thread::State::Exited);
        } else {
            libk_assert(owned && entity.state_ == Thread::State::Exited && entity.stopping_);
        }
    }
    if (queued && !kick_remote()) {
        panic("remote stop delivery failed");
    }
}

void Dispatcher::drain_remote() noexcept {
    libk_assert(!arch::interrupts_enabled());
    bool made_ready{};
    for (;;) {
        Sc* sc;
        u8 actions;
        {
            sync::Lock guard{mail_lock_};
            if (mail_.empty()) break;
            sc = &mail_.pop_front();
            actions = std::exchange(sc->actions_, u8{});
        }
        trace::emit(trace::Event::Take, reinterpret_cast<u64>(sc), actions);
        if (actions & Stop) {
            Thread* target = &sc->thread();
            const auto disposition = stop(target);
            // A stopping kernel continuation may still need the wake that
            // arrived with Stop to drain its operation before it can exit.
            if (disposition == StopDisposition::Deferred && (actions & Wake))
                made_ready = accept_wake(*sc) == WakeAcceptance::Readied || made_ready;
            // Complete consumer ownership before unbind can release Sc.
            complete(*sc);
            if (disposition == StopDisposition::Finalize)
                finish_exit(target, DispatchReason::Stop);
        } else {
            if (actions & Start) made_ready = make_ready(*sc) || made_ready;
            if (actions & Wake)
                made_ready = accept_wake(*sc) == WakeAcceptance::Readied || made_ready;
            complete(*sc);
        }
    }
    if (made_ready) {
        request_reschedule(DispatchReason::RemoteWake);
    }
}

void Dispatcher::enqueue_or_throttle(
    Sc& sc,
    time::Instant now) noexcept {
    Thread& exec = sc.thread();
    Sc& context = sc;
    libk_assert(!sc.queued() && !sc.timer_queued());
    if (context.eligible(now)) {
        exec.set_state(Thread::State::Ready);
        enqueue(sc);
        return;
    }
    const auto deadline = context.next_refill();
    libk_assert(deadline > now);
    exec.set_state(Thread::State::Throttled);
    timers_.insert(sc);
}

void Dispatcher::process_timers(time::Instant now) noexcept {
    for (;;) {
        Sc* const sc = timers_.minimum();
        if (!sc || sc->next_refill() > now) return;
        timers_.erase(*sc);
        libk_assert(sc->thread().state_
            == Thread::State::Throttled);
        enqueue_or_throttle(*sc, now);

    }
}

void Dispatcher::process_deadlines(time::Instant now) noexcept {
    for (;;) {
        Deadline* const deadline = deadlines_.minimum();
        if (!deadline || deadline->when_ > now) return;
        libk_assert(deadline->owner_ == this);
        deadlines_.erase(*deadline);
        deadline->owner_ = nullptr;
        const Deadline::Callback callback = deadline->callback_;
        libk_assert(callback);
        callback();
    }
}

void Dispatcher::charge_to(time::Instant now) noexcept {
    const auto elapsed = now.elapsed_since(accounted_at_);
    libk_assert(elapsed);
    if (current_sc_ != nullptr && !elapsed->empty()) {
        current_sc_->charge(now, *elapsed);
    }
    accounted_at_ = now;
}

void Dispatcher::yield() noexcept {
    libk_assert(!arch::interrupts_enabled());
    dispatch(DispatchReason::Yield, clock_->now());
}

void Dispatcher::block_current() noexcept {
    libk_assert(!arch::interrupts_enabled());
    libk_assert(cpu_.current && !cpu_.current->idle());
    libk_assert(current_sc_ != nullptr);
    if (current_sc_->wake_credit_) {
        current_sc_->wake_credit_ = false;

        return;
    }
    dispatch(DispatchReason::Block, clock_->now());
}

[[noreturn]] void Dispatcher::exit_current() noexcept {
    libk_assert(!arch::interrupts_enabled());
    libk_assert(cpu_.current && !cpu_.current->idle());
    dispatch(DispatchReason::Exit, clock_->now());
    libk_assert(false);
    __builtin_unreachable();
}

void Dispatcher::request_reschedule(DispatchReason reason) noexcept {
    if (!pending_ || reason == DispatchReason::Timer
        || reason == DispatchReason::Exit
        || reason == DispatchReason::Stop) {
        pending_ = reason;
        if (reason == DispatchReason::Exit) {
            pending_exit_status_ = STATUS_OK;
        }
    }
}

void Dispatcher::request_reschedule(
    DispatchReason reason,
    status_t exit_status) noexcept {
    if (!pending_ || reason == DispatchReason::Timer
        || reason == DispatchReason::Exit
        || reason == DispatchReason::Stop) {
        pending_ = reason;
        if (reason == DispatchReason::Exit) {
            pending_exit_status_ = exit_status;
        }
    }
}

void Dispatcher::on_timer() noexcept {
    libk_assert(!arch::interrupts_enabled());
    const time::Instant now = clock_->now();

    charge_to(now);
    arch::mask_timer();
    process_deadlines(now);
    request_reschedule(DispatchReason::Timer);
}

void Dispatcher::on_trap_exit() noexcept {
    libk_assert(!arch::interrupts_enabled());
    libk_assert(arch::local()->depth == 0);
    if (cpu_.current->stop_ready()) {
        request_reschedule(DispatchReason::Stop);
    }
    if (!pending_ || preempt_depth_ != 0
        || ((*pending_ == DispatchReason::Stop || *pending_ == DispatchReason::Exit)
            && cpu_.current->in_kernel_)) {
        return;
    }
    const DispatchReason reason = *pending_;
    const status_t exit_status = pending_exit_status_;
    pending_.reset();
    if (reason == DispatchReason::Block) {
        block_current();
        return;
    }
    dispatch(reason, clock_->now(), exit_status);
}

void Dispatcher::disable_preemption() noexcept {
    libk_assert(!arch::interrupts_enabled());
    ++preempt_depth_;
    libk_assert(preempt_depth_ != 0);
}

void Dispatcher::enable_preemption() noexcept {
    libk_assert(!arch::interrupts_enabled());
    libk_assert(preempt_depth_ != 0);
    --preempt_depth_;
    if (preempt_depth_ == 0 && pending_ && arch::local()->depth == 0) {
        on_trap_exit();
    }
}

void Dispatcher::dispatch(
    DispatchReason reason,
    time::Instant now,
    status_t exit_status) noexcept {
    libk_assert(!arch::interrupts_enabled());
    libk_assert(arch::local()->depth == 0);
    libk_assert(cpu_.current);
    charge_to(now);
    process_timers(now);

    Thread* outgoing = cpu_.current;
    Sc* const outgoing_sc = current_sc_;
    if (outgoing_sc != nullptr) {
        Sc& context = *outgoing_sc;
        context.deactivate(cpu_.id);
        current_sc_ = nullptr;
        switch (reason) {
        case DispatchReason::Exit:
        case DispatchReason::Stop:
            outgoing->set_state(Thread::State::Exited);
            break;
        case DispatchReason::Block:
            outgoing->set_state(Thread::State::Blocked);
            break;
        case DispatchReason::Start:
        case DispatchReason::Yield:
        case DispatchReason::Timer:
        case DispatchReason::RemoteWake:
            if (context.eligible(now)) {
                outgoing->set_state(Thread::State::Ready);
                enqueue(*outgoing_sc);

            } else {
                enqueue_or_throttle(*outgoing_sc, now);

            }
            break;
        }
    }

    Sc* const candidate = select();
    if (candidate == nullptr && outgoing->idle()) {
        outgoing->set_state(Thread::State::Running);
        program_deadline(now);
        record_dispatch(outgoing, outgoing, reason, now);
        return;
    }
    commit(candidate, reason, now, exit_status);

    if (reason == DispatchReason::Exit || reason == DispatchReason::Stop) {
        libk_assert(false);
        __builtin_unreachable();
    }
}

void Dispatcher::commit(
    Sc* candidate,
    DispatchReason reason,
    time::Instant now,
    status_t exit_status) noexcept {
    Thread* outgoing = cpu_.current;
    Thread* incoming = idle_;
    Sc* incoming_sc{};

    if (candidate != nullptr) {
        Sc& context = *candidate;
        Thread* target = &candidate->thread();
        Thread& exec = *target;
        libk_assert(candidate->home_cpu() == cpu_.id);
        libk_assert(candidate->queued());
        libk_assert(exec.state_ == Thread::State::Ready);
        libk_assert(exec.sc_ == candidate);
        libk_assert(context.bound());
        libk_assert(context.domain_ != nullptr);
        libk_assert(context.domain_->allows(cpu_.id));
        libk_assert(context.eligible(now));

        remove(*candidate);

        libk_assert(context.activate(cpu_.id));
        exec.set_state(Thread::State::Running);
        incoming = target;
        incoming_sc = candidate;
    } else {
        libk_assert(idle_->state_ == Thread::State::Prepared
            || idle_->state_ == Thread::State::Running);
        idle_->set_state(Thread::State::Running);
    }

    current_sc_ = incoming_sc;
    publish(incoming);
    program_deadline(now);
    record_dispatch(outgoing, incoming, reason, now);

    if (incoming == outgoing) {
        return;
    }
    if (outgoing->idle()) {
        outgoing->set_state(Thread::State::Prepared);
    }

    libk_assert(!handoff_outgoing_);
    handoff_outgoing_ = outgoing;
    handoff_reason_ = reason;
    handoff_exit_status_ = exit_status;
    sync::assert_unlocked();
    arch::switch_ctx(
        outgoing->ctx(), incoming->ctx());
    post_switch();
}

void Dispatcher::program_deadline(time::Instant now) noexcept {
    if (!timer_available_) {
        programmed_deadline_ = time::Instant::max();
        arch::mask_timer();
        return;
    }

    time::Instant deadline = time::Instant::max();
    if (current_sc_ != nullptr) {
        const time::Duration budget =
            current_sc_->available(now);
        const time::Duration slice = budget < quantum_ ? budget : quantum_;
        if (slice.empty()) {
            request_reschedule(DispatchReason::Timer);
            deadline = now;
        } else {
            const auto computed = now.checked_add(slice);
            libk_assert(computed);
            deadline = *computed;
        }
    }
    if (const auto* sc = timers_.minimum(); sc && sc->next_refill() < deadline)
        deadline = sc->next_refill();
    if (const auto* call = deadlines_.minimum(); call && call->when_ < deadline)
        deadline = call->when_;
    const auto programmed = arch::program_timer(deadline);
    if (!programmed) {
        timer_available_ = false;
        programmed_deadline_ = time::Instant::max();
        arch::mask_timer();
    } else {
        programmed_deadline_ = deadline;
    }
}

void Dispatcher::record_dispatch(
    Thread* outgoing,
    Thread* incoming,
    DispatchReason reason,
    time::Instant now) noexcept {
    trace::emit(trace::Event::Dispatch, outgoing->identity(), incoming->identity(),
                static_cast<u64>(reason), reinterpret_cast<u64>(current_sc_));
    static_cast<void>(now);
}

void Dispatcher::post_switch() noexcept {
    libk_assert(!arch::interrupts_enabled());
    libk_assert(cpu_.current);
    Thread* outgoing = handoff_outgoing_;
    const DispatchReason reason = handoff_reason_;
    const status_t exit_status = handoff_exit_status_;
    handoff_outgoing_ = {};
    handoff_exit_status_ = STATUS_OK;
    if (outgoing
        && outgoing->state_ == Thread::State::Exited) {
        finish_exit(outgoing, reason, exit_status);
    }
}

auto Dispatcher::stop(Thread* target) noexcept
    -> StopDisposition {
    libk_assert(!arch::interrupts_enabled());
    libk_assert(target->owned_by(*this));
    Thread& exec = *target;

    auto& wait = target->current_wait();
    if (wait.attached()) {
        if (!wait.cancel()) return StopDisposition::Deferred;
        if (exec.state_ == Thread::State::Blocked) {
            Sc* const sc = exec.sc_;
            libk_assert(sc != nullptr);
            static_cast<void>(accept_wake(*sc));
            return StopDisposition::Deferred;
        }
    }
    if (target->in_kernel_ || target->activation() != nullptr) {
        if (exec.state_ == Thread::State::Blocked) {
            libk_assert(exec.sc_ != nullptr);
            static_cast<void>(accept_wake(*exec.sc_));
        }
        return StopDisposition::Deferred;
    }

    if (exec.state_ == Thread::State::Running) {
        libk_assert(cpu_.current == target);
        request_reschedule(DispatchReason::Stop);
        return StopDisposition::Deferred;
    }
    if (exec.state_ == Thread::State::Blocked && target->draining()) {
        // The operation owns the continuation and may already be completing
        // on another CPU. Let its retained wake make the frame runnable; trap
        // exit consumes the result before the pending stop is committed.
        return StopDisposition::Deferred;
    }

    Sc* const sc = exec.sc_;
    if (sc != nullptr) {
        if (sc->queued()) {
            remove(*sc);
        }
        if (sc->timer_queued()) {
            timers_.erase(*sc);
        }

        // A claimed Stop still owns this Sc until complete().
        sync::Lock guard{mail_lock_};
        sc->actions_ = 0;
        if (sc->mail_hook_.is_linked()) {
            mail_.erase(*sc);
            sc->mailed_.store<libk::MemoryOrder::Release>(false);
        }
    }
    exec.set_state(Thread::State::Exited);
    return StopDisposition::Finalize;
}

void Dispatcher::cancel(Sc& sc) noexcept {
    libk_assert((arch::local() ? arch::local()->owner : nullptr) == &cpu_ && !arch::interrupts_enabled());
    sync::Lock guard{mail_lock_};
    if (sc.mail_hook_.is_linked()) mail_.erase(sc);
    else libk_assert(!sc.mailed_.load<libk::MemoryOrder::Relaxed>());
    sc.actions_ = 0;
    sc.mailed_.store<libk::MemoryOrder::Release>(false);
}

void Dispatcher::finish_exit(
    Thread* target,
    DispatchReason reason,
    status_t exit_status) noexcept {
    libk_assert(!arch::interrupts_enabled());
    Thread& exec = *target;
    libk_assert(exec.state_ == Thread::State::Exited);
    libk_assert(cpu_.current != target);
    // Also covers Stop of a ready exec after frame redirect: its saved
    // kernel stack is now discarded, so every popped stack can be recycled.
    target->release_calls();
    libk_assert(!target->draining());
    object::ref<> lifetime{};
    if (exec.sc_ != nullptr) {
        Sc& sc = *exec.sc_;
        libk_assert(!sc.queued() && !sc.timer_queued());
        auto unbound = sc.unbind(this);
        libk_assert(unbound);
        lifetime = std::move(unbound).value();
    }
    if (reason == DispatchReason::Stop) {
        target->finish_stop();
    } else {
        target->finish_exit(exit_status);
    }
}

void yield() noexcept {
    // The call may switch away before it returns.  An IrqToken's diagnostic
    // lifetime is stack-bound, so carrying it across that handoff would make
    // the next exec look as if this CPU still owned a disabled-IRQ
    // section.  Keep this scheduler boundary raw; the dispatcher itself
    // already requires interrupts to be masked.
    const bool interrupts = arch::disable_interrupts();
    Cpu& cpu = current_cpu();
    cpu.dispatcher().yield();
    arch::restore_interrupts(interrupts);
}

void block() noexcept {
    // See yield(): block_current() can hand the stack to another exec.
    const bool interrupts = arch::disable_interrupts();
    Cpu& cpu = current_cpu();
    cpu.dispatcher().block_current();
    arch::restore_interrupts(interrupts);
}

auto wake(
    Cpus& cpus,
    Sc& sc) noexcept
    -> Dispatcher::WakeResult {
    Cpu* const target = cpus.get(sc.home_cpu());
    if (target == nullptr
        || !target->online()) {
        return std::unexpected(Dispatcher::WakeError::Unavailable);
    }

    if ((arch::local() ? arch::local()->owner : nullptr) == target) {
        sync::Irq irq{};
        const Dispatcher::WakeAcceptance accepted =
            target->dispatcher().accept_wake(sc);
        if (accepted != Dispatcher::WakeAcceptance::Rejected) {
            return {};
        }
        return std::unexpected(Dispatcher::WakeError::Unavailable);
    }
    return target->dispatcher().post_wake(sc);
}

auto start(Cpus& cpus, Sc& sc) noexcept
    -> Dispatcher::WakeResult {
    Cpu* const target = cpus.get(sc.home_cpu());
    if (target == nullptr
        || !target->online()) {
        return std::unexpected(Dispatcher::WakeError::Unavailable);
    }
    if ((arch::local() ? arch::local()->owner : nullptr) == target) {
        sync::Irq irq{};
        const bool accepted = target->dispatcher().make_ready(sc);
        return accepted
            ? Dispatcher::WakeResult{}
            : Dispatcher::WakeResult{
                  std::unexpected(Dispatcher::WakeError::Unavailable)};
    }
    return target->dispatcher().post_start(sc);
}

[[noreturn]] void exit_current() noexcept {
    [[maybe_unused]] const bool interrupts =
        arch::disable_interrupts();
    Cpu& cpu = current_cpu();
    cpu.dispatcher().exit_current();
}

} // namespace sched
