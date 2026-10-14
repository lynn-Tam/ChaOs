#include <cpu/ipi.hpp>
#include <expected>
#include <bit>
#include <algorithm>
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
    const auto quantum = clock_->ticks<4, time::ms>();
    libk_assert(quantum && *quantum != 0);
    quantum_ = *quantum;
    has_timer_ = arch::timer_available();
    has_ipi_ = arch::ipi_available();
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

auto Dispatcher::remaining_budget() const noexcept -> u64 {
    libk_assert(current_sc_ != nullptr);
    return current_sc_->available(clock_->rd_ticks());
}

auto Dispatcher::current_urgency() const noexcept -> Urgency {
    libk_assert(current_sc_ != nullptr);
    return current_sc_->urgency();
}

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
    const u64 now = clock_->rd_ticks();
    if (sc.refill_.armed()) {
        if (exec.state_ != Thread::State::Throttled) {
            return false;
        }
        if (!context.eligible(now)) {
            return true;
        }
        disarm(sc.refill_);
    }
    enqueue_or_throttle(sc, now);

    if (exec.state_ == Thread::State::Throttled) {
        set_timer(now);
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
    if (!sc.mailed_.load(std::memory_order_relaxed)) {
        sc.mailed_.store(true, std::memory_order_release);
        mail_.push_back(sc);
    }
    trace::emit(trace::Event::Post, reinterpret_cast<u64>(&sc), actions);
}

void Dispatcher::complete(Sc& sc) noexcept {
    sync::Lock guard{mail_lock_};
    libk_assert(sc.mailed_.load(std::memory_order_relaxed) && !sc.mail_hook_.is_linked());
    if (sc.actions_) mail_.push_back(sc);
    else sc.mailed_.store(false, std::memory_order_release);
}

auto Dispatcher::post_remote(Sc& sc, u8 actions) noexcept -> WakeResult {
    if (!has_ipi_) return std::unexpected(WakeError::Unavailable);
    {
        // Serialize publication with external unbind. The binding keeps both
        // Sc and Thread resident until the home consumer completes its mail.
        sync::Lock guard{sc.lock_};
        if (!sc.owner_) return std::unexpected(WakeError::Unavailable);
        sync::Lock target_guard{sc.thread().lock_};
        if (sc.thread().sc_ != &sc) return std::unexpected(WakeError::Unavailable);
        post(sc, actions);
    }
    return kick_remote();
}

auto Dispatcher::kick_remote() noexcept -> WakeResult {
    if (!has_ipi_) {
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
    if (Cpu::local() == &cpu_) {
        sync::Irq irq{};
        // A target may finish its ordinary exit after the stop owner publishes
        // the terminal transaction but before this owner-CPU call is entered.
        // The queued Stop has already been completed by terminal cleanup then.
        if (target->stopped()) {
            return;
        }
        if (stop(target) == StopDisposition::Finalize) {
            finish_exit(target, DispatchReason::Stop);
        }
        return;
    }

    if (!has_ipi_) {
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
    u64 now) noexcept {
    Thread& exec = sc.thread();
    Sc& context = sc;
    libk_assert(!sc.queued() && !sc.refill_.armed());
    if (context.eligible(now)) {
        exec.set_state(Thread::State::Ready);
        enqueue(sc);
        return;
    }
    const auto deadline = context.next_refill();
    libk_assert(deadline > now);
    exec.set_state(Thread::State::Throttled);
    insert(sc.refill_, deadline);
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

void Dispatcher::charge_to(u64 now) noexcept {
    libk_assert(now >= accounted_at_);
    const u64 elapsed = now - accounted_at_;
    if (current_sc_ && elapsed != 0) current_sc_->charge(now, elapsed);
    accounted_at_ = now;
}

void Dispatcher::yield() noexcept {
    libk_assert(!arch::interrupts_enabled());
    dispatch(DispatchReason::Yield, clock_->rd_ticks());
}

void Dispatcher::block_current() noexcept {
    libk_assert(!arch::interrupts_enabled());
    libk_assert(cpu_.current && !cpu_.current->idle());
    libk_assert(current_sc_ != nullptr);
    dispatch(DispatchReason::Block, clock_->rd_ticks());
}

[[noreturn]] void Dispatcher::exit_current() noexcept {
    libk_assert(!arch::interrupts_enabled());
    libk_assert(cpu_.current && !cpu_.current->idle());
    dispatch(DispatchReason::Exit, clock_->rd_ticks());
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

void Dispatcher::dispatch(
    DispatchReason reason,
    u64 now,
    status_t exit_status) noexcept {
    libk_assert(!arch::interrupts_enabled());
    libk_assert(arch::local()->depth == 0);
    libk_assert(cpu_.current);
    charge_to(now);
    expire(now);
    // Expiry can complete the current Wait synchronously. Consume its wake
    // after publication, before committing the previously requested block.
    if (reason == DispatchReason::Block && std::exchange(current_sc_->wake_credit_, false)) {
        set_timer(now);
        return;
    }

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
        set_timer(now);
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
    u64 now,
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
    run_until_ = std::numeric_limits<u64>::max();
    if (incoming_sc) {
        const auto budget = incoming_sc->available(now);
        const auto end = libk::checked_add(now, std::min(budget, quantum_));
        libk_assert(end);
        run_until_ = *end;
    }
    // Check while current and stack bounds still describe the executing thread.
    sync::assert_unlocked();
    publish(incoming);
    set_timer(now);
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
    arch::switch_ctx(
        outgoing->ctx(), incoming->ctx());
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

auto Dispatcher::stop(Thread* target) noexcept
    -> StopDisposition {
    libk_assert(!arch::interrupts_enabled());
    libk_assert(target->owned_by(*this));
    Thread& exec = *target;

    auto& wait = target->wait();
    if (wait.attached()) {
        if (!wait.cancel()) return StopDisposition::Deferred;
        if (exec.state_ == Thread::State::Blocked) {
            Sc* const sc = exec.sc_;
            libk_assert(sc != nullptr);
            static_cast<void>(accept_wake(*sc));
            return StopDisposition::Deferred;
        }
    }
    if (target->in_kernel_) {
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
        if (sc->refill_.armed()) {
            disarm(sc->refill_);
        }

        // A claimed Stop still owns this Sc until complete().
        sync::Lock guard{mail_lock_};
        sc->actions_ = 0;
        if (sc->mail_hook_.is_linked()) {
            mail_.erase(*sc);
            sc->mailed_.store(false, std::memory_order_release);
        }
    }
    exec.set_state(Thread::State::Exited);
    return StopDisposition::Finalize;
}

void Dispatcher::cancel(Sc& sc) noexcept {
    libk_assert(Cpu::local() == &cpu_ && !arch::interrupts_enabled());
    sync::Lock guard{mail_lock_};
    if (sc.mail_hook_.is_linked()) mail_.erase(sc);
    else libk_assert(!sc.mailed_.load(std::memory_order_relaxed));
    sc.actions_ = 0;
    sc.mailed_.store(false, std::memory_order_release);
}

void Dispatcher::finish_exit(
    Thread* target,
    DispatchReason reason,
    status_t exit_status) noexcept {
    libk_assert(!arch::interrupts_enabled());
    Thread& exec = *target;
    libk_assert(exec.state_ == Thread::State::Exited);
    libk_assert(cpu_.current != target);
    libk_assert(!target->draining());
    object::ref<> lifetime{};
    if (exec.sc_ != nullptr) {
        Sc& sc = *exec.sc_;
        libk_assert(!sc.queued() && !sc.refill_.armed());
        auto unbound = sc.unbind(this);
        libk_assert(unbound);
        lifetime = std::move(unbound).value();
    }
    target->finish(reason == DispatchReason::Stop ? STATUS_CANCELED : exit_status);
}

void yield() noexcept {
    // Restore the caller IRQ state only when its own stack resumes.
    const bool interrupts = arch::disable_interrupts();
    Cpu& cpu = current_cpu();
    cpu.dispatcher().yield();
    arch::restore_interrupts(interrupts);
}

void block() noexcept {
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

    if (Cpu::local() == target) {
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
    if (Cpu::local() == target) {
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
