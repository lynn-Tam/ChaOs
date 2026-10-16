#include <sched/dispatcher.hpp>
#include <sched/sc.hpp>
#include <cpu/ipi.hpp>
#include <cpu/cpu.hpp>
#include <trace.hpp>
#include <panic.hpp>
#include <algorithm>

namespace sched {
Dispatcher::Dispatcher(Cpu& cpu, Thread& idle, time::Clock& clock) noexcept
    : cpu_(cpu), idle_(&idle), clock_(&clock),
      maintenance_({.budget = *clock.ticks<1, time::ms>(),
                    .period = *clock.ticks<10, time::ms>(), .weight = 1024}, clock.rd_ticks()) {
    libk_assert(idle.idle());
    quantum_ = *clock.ticks<4, time::ms>();
    has_timer_ = arch::timer_available();
    has_ipi_ = arch::ipi_available();
}
auto Dispatcher::id() const noexcept -> CpuId { return cpu_.id; }

auto Dispatcher::arm(
    Deadline& deadline,
    u64 when) noexcept -> bool {
    libk_assert(!arch::interrupts_enabled());
    libk_assert(Cpu::local() == &cpu_);
    if (!has_timer_ || deadline.armed() || !deadline.cb_) {
        return false;
    }
    insert(deadline, when);
    set_timer(clock_->rd_ticks());
    return true;
}

void Dispatcher::insert(Deadline& d, u64 when) noexcept {
    libk_assert(!d.armed());
    d.when_ = when;
    deadlines_.insert(d);
    d.owner_ = this;
}

void Dispatcher::disarm(Deadline& deadline) noexcept {
    libk_assert(!arch::interrupts_enabled());
    libk_assert(Cpu::local() == &cpu_);
    libk_assert(deadline.owner_ == this);
    deadlines_.erase(deadline);
    deadline.owner_ = nullptr;
    set_timer(clock_->rd_ticks());
}

void Dispatcher::publish(Thread* target) noexcept {
    libk_assert(!arch::interrupts_enabled());
    libk_assert(Cpu::local() == &cpu_);
    libk_assert(target);
    const usize stack_top = target->stack_top();
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
    accounted_at_ = clock_->rd_ticks();
    publish(idle_);
    set_timer(accounted_at_);
    Thread* idle = idle_;
    record_dispatch(idle, idle, DispatchReason::Start, accounted_at_);
    arch::enter_ctx(idle_->ctx());
}

void Dispatcher::on_context_enter() noexcept {
    libk_assert(!arch::interrupts_enabled());
    libk_assert(cpu_.current);
    libk_assert(cpu_.entry.stack
        == cpu_.current->stack_top());
    Env& roots = cpu_.current->env();
    libk_assert(cpu_.tlb
        == &roots.root().state());
    post_switch();
    if (has_ipi_) {
        arch::enable_ipi();
    }
    arch::enable_interrupts();
}


auto Dispatcher::maintenance(const Thread& t) const noexcept -> bool {
    return t.kind_ == Thread::Kind::Worker || t.stop_requested();
}
void Dispatcher::enqueue(Thread& t) noexcept {
    libk_assert(!t.ready_hook_.is_linked() && !t.maintenance_hook_.is_linked());
    t.set_state(Thread::State::Ready);
    if (maintenance(t)) maintenance_ready_.push_back(t);
    else {
        libk_assert(t.sc_);
        t.sc_->locate(*this);
        t.service_ = t.sc_->service_;
        t.enqueue_seq_ = ++enqueue_seq_;
        ready_.insert(t);
    }
    ++ready_count_;
}
void Dispatcher::remove(Thread& t) noexcept {
    if (t.ready_hook_.is_linked()) ready_.erase(t);
    else { libk_assert(t.maintenance_hook_.is_linked()); maintenance_ready_.erase(t); }
    --ready_count_;
}
auto Dispatcher::select(u64 now) noexcept -> Thread* {
    auto* candidate = ready_.minimum();
    if (!maintenance_ready_.empty()) {
        // One fair participant represents all cleanup continuations and work.
        maintenance_.service_ = std::max(maintenance_.service_, fair_.load());
        if (maintenance_.eligible(now)
            && (!candidate || maintenance_.service_ <= candidate->service_))
            return &maintenance_ready_.front();
        if (!maintenance_.eligible(now) && !maintenance_refill_.armed())
            insert(maintenance_refill_, maintenance_.next_refill());
    }
    return candidate;
}
void Dispatcher::refill_maintenance() noexcept {
    maintenance_.charge(clock_->rd_ticks(), 0);
    if (!maintenance_ready_.empty()) request_reschedule(DispatchReason::Timer);
}
void Dispatcher::detach(Thread& t) noexcept {
    object::ref<Sc> resource;
    {
        sync::Lock guard{t.lock_};
        libk_assert(!current_sc_ || current_sc_ != t.sc_);
        if (t.pending_sc_) resource = std::move(t.pending_sc_);
        else { resource = std::move(t.resource_); t.sc_ = nullptr; t.association_ = 0; }
        t.detach_sc_ = false;
        t.association_ = 0;
    }
    if (resource) resource->release(t);
}
void Dispatcher::refresh(Thread& t) noexcept {
    libk_assert(!arch::interrupts_enabled());
    if (t.state_ == Thread::State::Exited) return;
    if (&t != current()) request_reschedule(DispatchReason::RemoteWake);
    if (&t == current() && current_sc_) { request_reschedule(DispatchReason::RemoteWake); return; }
    if (t.ready_hook_.is_linked() || t.maintenance_hook_.is_linked()) remove(t);
    if (t.refill_.armed()) disarm(t.refill_);
    bool detach_sc, enabled, changed;
    {
        sync::Lock guard{t.lock_};
        detach_sc = t.detach_sc_;
        enabled = t.enabled_;
        changed = t.applied_ != t.requested_ || t.pending_sc_ || detach_sc;
        if (!detach_sc && t.pending_sc_) {
            t.resource_ = std::move(t.pending_sc_);
            t.sc_ = &t.resource_.get();
        }
        t.applied_ = t.requested_;
        t.applied_enabled_ = enabled;
    }
    if (detach_sc) detach(t);
    else if (t.sc_) {
        bool bound;
        { sync::Lock guard{t.sc_->lock_}; bound = t.sc_->applied_ != t.association_; t.sc_->applied_ = t.association_; }
        if (bound) static_cast<void>(t.sc_->changed_.signal());
    }
    if (changed) static_cast<void>(t.changed_.signal());
    if (maintenance(t)) {
        if (t.wait_.attached() && !t.wait_.ready() && !t.wake_credit_) {
            t.set_state(Thread::State::Blocked); return;
        }
        enqueue(t); return;
    }
    if (!enabled || !t.sc_ || (t.wait_.attached() && !t.wait_.ready() && !t.wake_credit_)) {
        t.set_state(Thread::State::Blocked); return;
    }
    auto& sc = *t.sc_;
    sc.charge(clock_->rd_ticks(), 0);
    if (sc.eligible(clock_->rd_ticks())) enqueue(t);
    else {
        t.set_state(Thread::State::Throttled);
        insert(t.refill_, sc.next_refill());
    }
}
void Dispatcher::request_control(Thread& t) noexcept {
    if (Cpu::local() == &cpu_ || !Cpu::local()) {
        sync::Irq irq{};
        refresh(t);
    } else { post(t, Control); static_cast<void>(kick_remote()); }
}
auto Dispatcher::accept_wake(Thread& t) noexcept -> WakeAcceptance {
    if (t.state_ == Thread::State::Exited) return WakeAcceptance::Rejected;
    if (t.state_ == Thread::State::Blocked) {
        // A wake which makes a blocked stack runnable is already consumed.
        t.wake_credit_ = true;
        refresh(t);
        t.wake_credit_ = false;
        return t.state_ == Thread::State::Ready ? WakeAcceptance::Readied : WakeAcceptance::Accepted;
    }
    t.wake_credit_ = true;
    return WakeAcceptance::Accepted;
}
void Dispatcher::post(Thread& t, u8 actions) noexcept {
    sync::Lock guard{mail_lock_};
    t.actions_ |= actions;
    if (!t.mailed_.exchange(true, std::memory_order_acq_rel)) {
        // This publisher is released only after the home finishes consuming.
        sync::Lock lifetime{t.grants_lock_};
        ++t.publishers_;
        mail_.push_back(t);
    }
}
void Dispatcher::complete(Thread& t) noexcept {
    bool finished;
    {
        sync::Lock guard{mail_lock_};
        libk_assert(!t.mail_hook_.is_linked());
        finished = !t.actions_;
        if (finished) t.mailed_.store(false, std::memory_order_release);
        else mail_.push_back(t);
    }
    if (finished) t.finish_retire();
}
auto Dispatcher::kick_remote() noexcept -> WakeResult {
    if (Cpu::local() == &cpu_) { request_reschedule(DispatchReason::RemoteWake); return {}; }
    if (!cpu_.online()) return {};
    if (!has_ipi_) return std::unexpected(WakeError::Unavailable);
    for (usize i = 0; i < 8; ++i) if (send_ipi(cpu_.hw)) return {};
    panic("IPI delivery failed");
}
auto Dispatcher::post_wake(Thread& t) noexcept -> WakeResult {
    post(t, Wake);
    return kick_remote();
}
void Dispatcher::request_stop(Thread& t) noexcept {
    // Even local Stop is mailed: callbacks can retire their target at return.
    post(t, Stop);
    if (Cpu::local() == &cpu_) { sync::Irq irq{}; drain_remote(); }
    else static_cast<void>(kick_remote());
}
void Dispatcher::drain_remote() noexcept {
    for (;;) {
        Thread* t;
        u8 actions;
        {
            sync::Lock guard{mail_lock_};
            if (mail_.empty()) break;
            t = &mail_.pop_front();
            actions = std::exchange(t->actions_, u8{});
        }
        bool finalize = false;
        if (actions & Stop) finalize = stop(t) == StopDisposition::Finalize;
        if (!finalize && t->state_ != Thread::State::Exited) {
            if (actions & Control) refresh(*t);
            if (actions & Wake) static_cast<void>(accept_wake(*t));
        }
        if (finalize) finish_exit(t, DispatchReason::Stop);
        complete(*t); // Last access: this may release Thread storage.
    }
}
void Dispatcher::expire(u64 now) noexcept {
    // Completion only requests a wake; dispatch remains at the outer boundary.
    libk_assert(!arch::interrupts_enabled());
    for (;;) {
        Deadline* const deadline = deadlines_.minimum();
        if (!deadline || deadline->when_ > now) return;
        libk_assert(deadline->owner_ == this);
        deadlines_.erase(*deadline);
        deadline->owner_ = nullptr;
        const Deadline::Fn cb = deadline->cb_;
        libk_assert(cb);
        cb();
    }
}


auto Dispatcher::current() const noexcept -> Thread* { return cpu_.current; }
auto Dispatcher::remaining_budget() const noexcept -> u64 {
    return current_sc_ ? current_sc_->available(clock_->rd_ticks()) : 0;
}
void Dispatcher::charge_to(u64 now) noexcept {
    const auto elapsed = now - accounted_at_;
    if (current_sc_) {
        current_sc_->charge(now, elapsed);
        {
            sync::Lock guard{cpu_.current->lock_};
            cpu_.current->runtime_ += elapsed;
            cpu_.current->accounted_at_ = now;
        }
        auto next = current_sc_->service_;
        if (const auto* t = ready_.minimum()) next = std::min(next, t->service_);
        if (!maintenance_ready_.empty() && maintenance_.eligible(now))
            next = std::min(next, maintenance_.service_);
        fair_.store(std::max(fair_.load(), next));
    }
    accounted_at_ = now;
}
void Dispatcher::yield() noexcept {
    drain_remote();
    dispatch(current()->stop_ready() ? DispatchReason::Stop : DispatchReason::Yield, clock_->rd_ticks());
}
void Dispatcher::block_current() noexcept {
    libk_assert(cpu_.current && !cpu_.current->idle());
    dispatch(DispatchReason::Block, clock_->rd_ticks());
}
[[noreturn]] void Dispatcher::exit_current() noexcept {
    dispatch(DispatchReason::Exit, clock_->rd_ticks());
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
    const u64 now = clock_->rd_ticks();

    charge_to(now);
    arch::mask_timer();
    expire(now);
    request_reschedule(DispatchReason::Timer);
}

void Dispatcher::resched() noexcept {
    libk_assert(!arch::interrupts_enabled());
    libk_assert(arch::local()->depth == 0);
    // post_switch may finish an exit and publish another local wake after
    // selecting idle. Consume that intent before returning to the caller.
    for (;;) {
        drain_remote();
        if (cpu_.current->stop_ready()) request_reschedule(DispatchReason::Stop);
        if (!pending_ ||
            ((*pending_ == DispatchReason::Stop || *pending_ == DispatchReason::Exit)
                && cpu_.current->in_kernel_)) return;
        const auto reason = *pending_;
        const auto status = pending_exit_status_;
        pending_.reset();
        if (reason == DispatchReason::Block) block_current();
        else dispatch(reason, clock_->rd_ticks(), status);
    }
}

void Dispatcher::dispatch(DispatchReason reason, u64 now, status_t status) noexcept {
    libk_assert(!arch::interrupts_enabled() && arch::local()->depth == 0);
    charge_to(now);
    expire(now);
    auto& outgoing = *current();
    // A wake between a waiter's arm and block survives intervening preemption
    // or yield. Only the attempted block consumes that single credit.
    const bool credit = reason == DispatchReason::Block && std::exchange(outgoing.wake_credit_, false);
    current_sc_ = nullptr;
    if (!outgoing.idle()) {
        if (reason == DispatchReason::Exit || reason == DispatchReason::Stop) {
            libk_assert(!outgoing.in_kernel_);
            outgoing.set_state(Thread::State::Exited);
        } else if (reason == DispatchReason::Block && !credit) {
            outgoing.set_state(Thread::State::Blocked);
            // An explicit block without a Wait (worker) waits for its wake.
            // Control can still detach resources without waking the stack.
            bool detach_sc;
            { sync::Lock guard{outgoing.lock_}; detach_sc = outgoing.detach_sc_; }
            if (detach_sc) detach(outgoing);
        } else refresh(outgoing);
    }
    // Refresh may cross a refill boundary. Select and commit against a sample
    // after those updates, never an older time than the queued eligibility.
    now = clock_->rd_ticks();
    accounted_at_ = now;
    auto* candidate = select(now);
    if (!candidate && outgoing.idle()) { set_timer(now); return; }
    commit(candidate, reason, now, status);
    if (reason == DispatchReason::Exit || reason == DispatchReason::Stop) __builtin_unreachable();
}
void Dispatcher::commit(Thread* candidate, DispatchReason reason, u64 now, status_t status) noexcept {
    Thread* outgoing = current();
    Thread* incoming = candidate ? candidate : idle_;
    if (candidate) {
        remove(*candidate);
        current_sc_ = maintenance(*candidate) ? &maintenance_ : candidate->sc_;
        libk_assert(current_sc_ && current_sc_->eligible(now));
        candidate->set_state(Thread::State::Running);
    } else idle_->set_state(Thread::State::Running);
    run_until_ = current_sc_ ? now + std::min(quantum_, current_sc_->available(now))
                            : std::numeric_limits<u64>::max();
    sync::assert_unlocked();
    publish(incoming);
    set_timer(now);
    record_dispatch(outgoing, incoming, reason, now);
    if (incoming == outgoing) return;
    if (outgoing->idle()) outgoing->set_state(Thread::State::Prepared);
    libk_assert(!handoff_outgoing_);
    handoff_outgoing_ = outgoing;
    handoff_reason_ = reason;
    handoff_exit_status_ = status;
    arch::switch_ctx(outgoing->ctx(), incoming->ctx());
    post_switch();
}
void Dispatcher::set_timer(u64 now) noexcept {
    if (!has_timer_) {
        arch::mask_timer();
        return;
    }

    // arm/disarm must not extend the current dispatch's execution grant.
    auto deadline = run_until_;
    if (current_sc_ && deadline <= now) request_reschedule(DispatchReason::Timer);
    if (const auto* d = deadlines_.minimum()) deadline = std::min(deadline, d->when_);
    deadline = std::max(deadline, now);
    const auto programmed = arch::program_timer(deadline);
    if (!programmed) {
        has_timer_ = false;
        arch::mask_timer();
    }
}

void Dispatcher::record_dispatch(
    Thread* outgoing,
    Thread* incoming,
    DispatchReason reason,
    u64 now) noexcept {
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

auto Dispatcher::stop(Thread* t) noexcept -> StopDisposition {
    libk_assert(t->owned_by(*this));
    if (t->state_ == Thread::State::Exited) return StopDisposition::Deferred;
    if (t->wait_.attached()) static_cast<void>(t->wait_.cancel());
    if (t->in_kernel_) {
        // Only a completed/canceled operation can make a blocked frame drain.
        if (!t->wait_.attached() || t->wait_.ready()) {
            t->wake_credit_ = true;
            refresh(*t);
        }
        return StopDisposition::Deferred;
    }
    if (t == current()) { request_reschedule(DispatchReason::Stop); return StopDisposition::Deferred; }
    if (t->ready_hook_.is_linked() || t->maintenance_hook_.is_linked()) remove(*t);
    if (t->refill_.armed()) disarm(t->refill_);
    t->set_state(Thread::State::Exited);
    return StopDisposition::Finalize;
}
void Dispatcher::finish_exit(Thread* t, DispatchReason reason, status_t status) noexcept {
    libk_assert(t != current() && t->state_ == Thread::State::Exited && !t->draining());
    if (t->refill_.armed()) disarm(t->refill_);
    detach(*t);
    t->finish(reason == DispatchReason::Stop ? STATUS_CANCELED : status);
}
void yield() noexcept {
    const bool interrupts = arch::disable_interrupts();
    current_cpu().dispatcher().yield();
    arch::restore_interrupts(interrupts);
}
void block() noexcept {
    const bool interrupts = arch::disable_interrupts();
    current_cpu().dispatcher().block_current();
    arch::restore_interrupts(interrupts);
}
auto wake(Cpus& cpus, Thread& thread) noexcept -> Dispatcher::WakeResult {
    auto* cpu = cpus.get(thread.home_cpu());
    if (!cpu || !cpu->online()) return std::unexpected(Dispatcher::WakeError::Unavailable);
    if (cpu == Cpu::local()) {
        sync::Irq irq{};
        return cpu->dispatcher().accept_wake(thread) != Dispatcher::WakeAcceptance::Rejected
            ? Dispatcher::WakeResult{} : std::unexpected(Dispatcher::WakeError::Unavailable);
    }
    return cpu->dispatcher().post_wake(thread);
}
[[noreturn]] void exit_current() noexcept {
    static_cast<void>(arch::disable_interrupts());
    current_cpu().dispatcher().exit_current();
}
} // namespace sched
