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
      maintenance_({.weight = 1024}, clock.rd_ticks()) {
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
    const usize stack_top = target->transient() ? cpu_.init_stack->top() : target->stack_top();
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
    recorded_at_ = clock_->rd_ticks();
    publish(idle_);
    set_timer(recorded_at_);
    Thread* idle = idle_;
    record_dispatch(idle, idle, DispatchReason::Start, recorded_at_);
    arch::enter_ctx(idle_->ctx());
}

void Dispatcher::on_context_enter() noexcept {
    libk_assert(!arch::interrupts_enabled());
    libk_assert(cpu_.current);
    libk_assert(cpu_.entry.stack
        == (cpu_.current->transient() ? cpu_.init_stack->top() : cpu_.current->stack_top()));
    Env& roots = cpu_.current->env();
    libk_assert(cpu_.tlb
        == &roots.root().state());
    post_switch();
    if (has_ipi_) {
        arch::enable_ipi();
    }
    if (!cpu_.current->transient()) arch::enable_interrupts();
}


auto Dispatcher::maintenance(const Thread& t) const noexcept -> bool {
    return (t.kind_ == Thread::Kind::Worker && !t.work_domain_) || t.stop_requested();
}
void Dispatcher::enqueue(Thread& t) noexcept {
    libk_assert(!t.ready_.hook.is_linked() && !t.maintenance_hook_.is_linked());
    t.set_state(Thread::State::Ready);
    if (maintenance(t)) maintenance_ready_.push_back(t);
    else {
        libk_assert(t.sc_ || t.work_domain_);
        if (t.sc_) { t.sc_->locate(*this); t.ready_.service = t.sc_->service_; }
        t.ready_.domain = t.work_domain_ ? t.work_domain_ : t.sc_->domain_;
        t.ready_.parent = &t.ready_.domain->local(id()).node;
        insert(t.ready_);
    }
    ++ready_count_;
}
void Dispatcher::remove(Thread& t) noexcept {
    if (t.ready_.hook.is_linked()) erase(t.ready_);
    else { libk_assert(t.maintenance_hook_.is_linked()); maintenance_ready_.erase(t); }
    --ready_count_;
}
void Dispatcher::insert(Node& node) noexcept {
    libk_assert(!node.hook.is_linked());
    auto& queue = node.parent ? node.parent->ready : ready_;
    node.service = std::max(node.service, node.parent ? node.parent->floor : fair_.load());
    node.sequence = ++enqueue_seq_;
    queue.insert(node);
    if (node.parent && !node.parent->hook.is_linked()) {
        auto& local = node.parent->domain->local(id());
        if (!local.refill.armed() && !local.stopped && local.owner.accepting()) insert(*node.parent);
    }
}
void Dispatcher::erase(Node& node) noexcept {
    auto& queue = node.parent ? node.parent->ready : ready_;
    queue.erase(node);
    if (node.parent && queue.empty()) {
        auto& local = node.parent->domain->local(id());
        if (local.refill.armed()) disarm(local.refill);
        if (node.parent->hook.is_linked()) erase(*node.parent);
    }
}
void Dispatcher::domain_ready(Domain::Local& local) noexcept {
    libk_assert(local.cpu == id() && !arch::interrupts_enabled());
    local.waiting.store(false, std::memory_order_release);
    if (!local.owner.accepting()) {
        if (local.refill.armed()) disarm(local.refill);
        if (local.node.hook.is_linked()) erase(local.node);
        libk_assert(!local.active);
        if (local.owner.phase_ == DOMAIN_CONFIGURING && local.owner.relocating_) {
            auto* parent = &local.owner.next_parent_->local(id()).node;
            if (local.node.parent != parent) {
                const auto lag = local.node.service > local.node.parent->floor
                    ? local.node.service - local.node.parent->floor : 0;
                local.node.parent = parent;
                local.node.service = parent->floor + lag;
            }
        }
        local.stopped.store(true, std::memory_order_release);
    } else {
        local.stopped.store(false, std::memory_order_release);
        local.node.fraction = local.node.fraction * local.owner.config_.weight / local.node.weight;
        local.node.weight = local.owner.config_.weight;
        const bool allowed = id().raw < 64 && (local.owner.config_.cpus & (u64{1} << id().raw));
        if (!allowed) local.owner.ordinary_.disarm(id());
        if (!allowed && local.worker.enabled_) static_cast<void>(local.worker.execution(false));
        if (allowed && !local.owner.ordinary_.arm(id())) local.owner.wake_work(id());
        if (!local.node.ready.empty() && !local.node.hook.is_linked()) insert(local.node);
    }
    request_reschedule(DispatchReason::RemoteWake);
}
void Dispatcher::post(Domain::Local& local) noexcept {
    auto ticket = object::ref<>::retain_alive(local.owner.close_work_.key());
    { sync::Lock guard{mail_lock_};
      if (!local.mailed) {
          local.mailed = true; local.ticket = std::move(ticket);
          local.owner.pending_mail_.fetch_add(1, std::memory_order_relaxed);
          domains_.push_back(local);
      }
    }
    static_cast<void>(kick_remote());
}
void Dispatcher::drain_domains() noexcept {
    libk_assert(arch::local()->depth == 0);
    for (;;) {
        Domain::Local* local;
        object::ref<> ticket;
        { sync::Lock guard{mail_lock_};
          if (domains_.empty()) return;
          local = &domains_.pop_front(); local->mailed = false;
          ticket = std::move(local->ticket);
        }
        domain_ready(*local);
        local->owner.pending_mail_.fetch_sub(1, std::memory_order_release);
        local->owner.post_close();
    }
}
auto Dispatcher::reserve(Thread& candidate, u64 now) noexcept -> bool {
    libk_assert(!current_sc_ && !ticket_count_);
    auto* sc = maintenance(candidate) ? &maintenance_ : candidate.sc_;
    auto* branch = maintenance(candidate) ? nullptr : candidate.ready_.parent;
    std::array<Node*, Domain::max_depth> path{};
    usize depth{};
    for (auto* node = branch; node; node = node->parent) path[depth++] = node;
    auto duration = quantum_;
    for (usize i = depth; i; --i) {
        auto& domain = *path[i - 1]->domain;
        bool blocked = !domain.accepting();
        if (!blocked && domain.config_.budget) {
            // Capped ancestors share one ledger across all homes. Larger
            // earlier reservations remain escrow until this real boundary.
            domain.local(id()).waiting.store(true, std::memory_order_release);
            // A shared small cap must not routinely hand its entire period
            // to the first home. Finite slices leave credit for peers; an
            // otherwise idle home can borrow the remaining slices itself.
            const auto slice = std::max<u64>(1, domain.config_.budget / std::popcount(domain.config_.cpus));
            auto loan = domain.budget_.reserve(now, std::min(duration, slice));
            tickets_[ticket_count_++] = {&domain, loan};
            duration = std::min(duration, loan.amount);
            blocked = !duration;
            if (!blocked) domain.local(id()).waiting.store(false, std::memory_order_release);
        }
        if (blocked) {
            auto& local = domain.local(id());
            if (local.node.hook.is_linked()) erase(local.node);
            if (domain.accepting() && !local.refill.armed()) {
                const auto when = domain.budget_.next_refill();
                if (when != std::numeric_limits<u64>::max()) insert(local.refill, when);
            }
            settle(now);
            if (domain.config_.budget && domain.budget_.info(now).available) domain.notify();
            return false;
        }
    }
    loan_ = sc ? sc->budget_.reserve(now, duration) : Budget::Loan{};
    duration_ = sc && sc->config_.budget ? loan_.amount : duration;
    libk_assert(duration_);
    current_sc_ = sc; current_branch_ = branch;
    return true;
}
void Dispatcher::settle(u64 now) noexcept {
    const auto elapsed = std::exchange(loan_elapsed_, 0);
    if (current_sc_) current_sc_->budget_.settle(std::exchange(loan_, {}), now, elapsed);
    while (ticket_count_) {
        const auto ticket = tickets_[--ticket_count_];
        ticket.domain->budget_.settle(ticket.loan, now, elapsed);
        // A CPU blocked solely by outstanding escrow needs a real return
        // event; an arbitrary timer retry could spend the same credit twice.
        if (ticket.domain->accepting() && ticket.domain->budget_.info(now).available) ticket.domain->notify();
    }
}
auto Dispatcher::select(u64 now) noexcept -> Thread* {
    for (;;) {
        auto* candidate = ready_.minimum();
        if (!maintenance_ready_.empty()) {
            maintenance_.service_ = std::max(maintenance_.service_, fair_.load());
            if (!candidate || maintenance_.service_ <= candidate->service) {
                auto& thread = maintenance_ready_.front();
                libk_assert(reserve(thread, now)); return &thread;
            }
        }
        while (candidate && !candidate->thread) candidate = candidate->ready.minimum();
        if (!candidate) return nullptr;
        auto& thread = *candidate->thread;
        // These execution relations were indexed before close; admission
        // checks precede a finite step and its home owns the eventual ACK.
        if (!thread.stop_requested() && !object::ref<>::admitting(thread.finalizer_.key())) {
            auto storage = object::ref<>::retain_alive(thread.finalizer_.key());
            thread.request_stop(); continue;
        }
        if (!maintenance(thread) && thread.sc_ && !object::ref<>::admitting(thread.sc_->close_work_.key())) {
            { sync::Lock guard{thread.lock_}; thread.detach_sc_ = true; }
            refresh(thread); continue;
        }
        if (reserve(thread, now)) return candidate->thread;
    }
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
    if (&t == current() && (current_sc_ || current_branch_)) { request_reschedule(DispatchReason::RemoteWake); return; }
    if (t.ready_.hook.is_linked() || t.maintenance_hook_.is_linked()) remove(t);
    if (t.refill_.armed()) disarm(t.refill_);
    if (t.wait_when_) {
        if (t.wait_.attached() && !t.wait_.ready() && t.wait_.source_.source_)
            libk_assert(arm(t.wait_.source_.deadline_, *t.wait_when_));
        t.wait_when_.reset();
    }
    bool detach_sc, enabled, changed;
    {
        sync::Lock guard{t.lock_};
        detach_sc = t.detach_sc_;
        enabled = t.enabled_;
        changed = t.applied_ != t.requested_ || t.pending_sc_ || detach_sc || t.transferred_;
        if (t.transferred_) { t.transferred_ = false; t.destination_ = nullptr; }
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
        if (bound) t.sc_->state_changed();
    }
    if (changed) t.state_changed();
    if (maintenance(t)) {
        if (t.wait_.attached() && !t.wait_.ready() && !t.wake_credit_) {
            t.set_state(Thread::State::Blocked); return;
        }
        enqueue(t); return;
    }
    if (!enabled || (!t.sc_ && !t.work_domain_) || (t.wait_.attached() && !t.wait_.ready() && !t.wake_credit_)) {
        t.set_state(Thread::State::Blocked); return;
    }
    if (t.work_domain_) { enqueue(t); return; }
    auto& sc = *t.sc_;
    sc.charge(clock_->rd_ticks(), 0);
    if (sc.eligible(clock_->rd_ticks())) enqueue(t);
    else {
        t.set_state(Thread::State::Throttled);
        insert(t.refill_, sc.next_refill());
    }
}
void Dispatcher::request_control(Thread& t) noexcept {
    post(t, Control); static_cast<void>(kick_remote());
}
auto Dispatcher::accept_wake(Thread& t) noexcept -> WakeAcceptance {
    if (t.home_.load(std::memory_order_acquire) != this) { post(t, Wake); return WakeAcceptance::Accepted; }
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
    Dispatcher* destination;
    { sync::Lock guard{mail_lock_};
      destination = t.home_.load(std::memory_order_acquire);
      if (destination == this) {
          t.actions_ |= actions;
          if (!t.mailed_.exchange(true, std::memory_order_acq_rel)) {
              t.mail_hold_.emplace(t.roots_.hold()); mail_.push_back(t);
          }
          return;
      }
    }
    // A stale route carries only intent. The new home alone mutates execution
    // state; the old mailbox lock serializes this check with ownership transfer.
    if (destination) { destination->post(t, actions); static_cast<void>(destination->kick_remote()); }
}
auto Dispatcher::migrate(Thread& t) noexcept -> bool {
    Dispatcher* destination;
    { sync::Lock guard{t.lock_};
      sync::Lock mailbox{mail_lock_};
      if (!t.destination_ || t.transferred_ || t.home_ != this || t.stopping_ || t.stopped_
          || t.in_kernel_ || t.mailed_ || t.actions_) return false;
      destination = t.destination_;
    }
    auto storage = object::ref<>::retain_alive(t.finalizer_.key());
    const bool ready = t.ready_.hook.is_linked() || t.maintenance_hook_.is_linked();
    const auto refill = t.refill_.armed() ? std::optional{t.refill_.when_} : std::nullopt;
    if (ready) remove(t);
    if (refill) disarm(t.refill_);
    auto& deadline = t.wait_.source_.deadline_;
    if (deadline.armed()) { t.wait_when_ = deadline.when_; disarm(deadline); }
    // Binding remains exclusive; translate its fair lag at the new home.
    if (t.sc_) {
        sync::Lock guard{t.sc_->lock_};
        const auto floor = t.sc_->domain_->local(id()).node.floor;
        t.sc_->lag_ = t.sc_->service_ > floor ? t.sc_->service_ - floor : 0;
    }
    bool committed;
    { sync::Lock guard{t.lock_};
      sync::Lock mailbox{mail_lock_};
      committed = !t.mailed_ && !t.actions_ && !t.stopping_ && !t.stopped_;
      if (committed) {
          if (&t == current()) { libk_assert(!departure_); departure_ = std::move(storage); }
          t.state_ = Thread::State::Blocked;
          t.cpu_ = destination->id(); ++t.home_epoch_; t.transferred_ = true;
          t.home_.store(destination, std::memory_order_release);
      }
    }
    if (!committed) {
        // Wake alone need not refresh a Thread already marked Ready. Restore
        // the exact old home entries before letting that producer run.
        if (t.wait_when_) { libk_assert(arm(deadline, *t.wait_when_)); t.wait_when_.reset(); }
        if (refill) libk_assert(arm(t.refill_, *refill));
        if (ready) enqueue(t);
        return false;
    }
    destination->post(t, Control); static_cast<void>(destination->kick_remote());
    return true;
}
void Dispatcher::complete(Thread& t) noexcept {
    std::optional<cap::Bindings<2>::Hold> publishing;
    { sync::Lock guard{mail_lock_};
      libk_assert(!t.mail_hook_.is_linked());
      if (!t.actions_) {
          libk_assert(t.mail_hold_);
          publishing.emplace(std::move(*t.mail_hold_)); t.mail_hold_.reset();
          t.mailed_.store(false, std::memory_order_release);
      } else mail_.push_back(t);
    }
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
        auto storage = object::ref<>::retain_alive(t->finalizer_.key());
        bool finalize = false;
        if (actions & Stop) finalize = stop(t) == StopDisposition::Finalize;
        if (!finalize && t->state_ != Thread::State::Exited) {
            if (actions & Control) refresh(*t);
            if (actions & Wake) static_cast<void>(accept_wake(*t));
        }
        if (finalize) finish_exit(t, DispatchReason::Stop);
        complete(*t);
        if (!finalize && t != current()) static_cast<void>(migrate(*t));
    }
    // Domain mail requires a real boundary even when the receiving CPU is
    // idle with no timer armed. An IPI is a request, never the stop ACK.
    { sync::Lock guard{mail_lock_};
      if (!domains_.empty()) request_reschedule(DispatchReason::RemoteWake);
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
    const auto now = clock_->rd_ticks();
    return (current_sc_ || current_branch_) && run_until_ > now ? run_until_ - now : 0;
}
void Dispatcher::charge_to(u64 now) noexcept {
    const auto elapsed = now - recorded_at_;
    if (current_sc_ || current_branch_) {
        if (current_sc_) current_sc_->record(elapsed);
        loan_elapsed_ += elapsed;
        {
            sync::Lock guard{cpu_.current->lock_};
            cpu_.current->runtime_ += elapsed;
            cpu_.current->recorded_at_ = now;
        }
        auto* child = &cpu_.current->ready_;
        if (current_sc_) child->service = current_sc_->service_;
        else child->service += elapsed;
        for (auto* branch = current_branch_; branch; branch = branch->parent) {
            branch->domain->runtime_.fetch_add(elapsed, std::memory_order_relaxed);
            if (cpu_.current->work_domain_) branch->domain->work_runtime_.fetch_add(elapsed, std::memory_order_relaxed);
            auto minimum = child->service;
            if (const auto* other = branch->ready.minimum()) minimum = std::min(minimum, other->service);
            branch->floor = std::max(branch->floor, minimum);
            auto& queue = branch->parent ? branch->parent->ready : ready_;
            const auto linked = branch->hook.is_linked();
            if (linked) queue.erase(*branch);
            const auto scaled = static_cast<unsigned __int128>(elapsed) * 1024 + branch->fraction;
            branch->service += static_cast<u64>(scaled / branch->weight);
            branch->fraction = static_cast<u64>(scaled % branch->weight);
            if (linked) queue.insert(*branch);
            child = branch;
        }
        auto next = current_branch_ ? child->service : current_sc_->service_;
        if (const auto* node = ready_.minimum()) next = std::min(next, node->service);
        if (!maintenance_ready_.empty())
            next = std::min(next, maintenance_.service_);
        fair_.store(std::max(fair_.load(), next));
    }
    recorded_at_ = now;
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
    settle(now);
    for (auto* branch = current_branch_; branch; branch = branch->parent)
        branch->domain->local(id()).active.store(false, std::memory_order_release);
    current_branch_ = nullptr;
    current_sc_ = nullptr;
    drain_domains();
    const bool moved = reason != DispatchReason::Exit && reason != DispatchReason::Stop && migrate(outgoing);
    if (!outgoing.idle() && !moved) {
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
    recorded_at_ = now;
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
        // Reserve fixes the quota and path. A concurrent Stop may change the
        // next admission, but cannot move this outstanding loan to maintenance.
        libk_assert(duration_ && ((current_sc_ && loan_.generation) || current_branch_));
        for (auto* branch = current_branch_; branch; branch = branch->parent)
            branch->domain->local(id()).active.store(true, std::memory_order_release);
        candidate->set_state(Thread::State::Running);
    } else idle_->set_state(Thread::State::Running);
    run_until_ = std::numeric_limits<u64>::max();
    if (current_sc_ || current_branch_) {
        run_until_ = libk::checked_add(now, duration_).value_or(std::numeric_limits<u64>::max());
    }
    sync::assert_unlocked();
    publish(incoming);
    set_timer(now);
    record_dispatch(outgoing, incoming, reason, now);
    if (incoming == outgoing) return;
    if (outgoing->idle()) outgoing->set_state(Thread::State::Prepared);
    libk_assert(!handoff_outgoing_);
    handoff_outgoing_ = departure_ ? nullptr : outgoing;
    handoff_reason_ = reason;
    handoff_exit_status_ = status;
    if (incoming->transient()) cpu_.step_ctx = arch::Ctx{cpu_.init_stack->top(), &Thread::start, incoming};
    auto& next = incoming->transient() ? cpu_.step_ctx : incoming->ctx();
    // Transient steps have returned; no C++ continuation may leave this CPU.
    if (outgoing->transient()) arch::enter_ctx(next);
    arch::switch_ctx(outgoing->ctx(), next);
    post_switch();
}
void Dispatcher::set_timer(u64 now) noexcept {
    if (!has_timer_) {
        arch::mask_timer();
        return;
    }

    // arm/disarm must not extend the current dispatch's execution grant.
    auto deadline = run_until_;
    if ((current_sc_ || current_branch_) && deadline <= now) request_reschedule(DispatchReason::Timer);
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
    departure_.reset();
    if (outgoing
        && outgoing->state_ == Thread::State::Exited) {
        finish_exit(outgoing, reason, exit_status);
    }
}

auto Dispatcher::stop(Thread* t) noexcept -> StopDisposition {
    libk_assert(t->owned_by(*this));
    if (t->state_ == Thread::State::Exited) return StopDisposition::Deferred;
    if (t->wait_.attached()) static_cast<void>(t->wait_.cancel());
    if (t->draining()) {
        // The fixed Wait must drain before Stop may finish its Thread.
        if (!t->wait_.attached() || t->wait_.ready()) {
            t->wake_credit_ = true;
            refresh(*t);
        }
        return StopDisposition::Deferred;
    }
    if (t == current()) { request_reschedule(DispatchReason::Stop); return StopDisposition::Deferred; }
    if (t->ready_.hook.is_linked() || t->maintenance_hook_.is_linked()) remove(*t);
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
    // Publication callbacks can run inside refresh(). Even a local wake is
    // intent; only the home boundary may mutate run queues.
    return cpu->dispatcher().post_wake(thread);
}
[[noreturn]] void exit_current() noexcept {
    static_cast<void>(arch::disable_interrupts());
    current_cpu().dispatcher().exit_current();
}
} // namespace sched
