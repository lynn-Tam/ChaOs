#include <sched/sc.hpp>
#include <op.hpp>
#include <sched/dispatcher.hpp>
#include <cpu/cpu.hpp>
#include <algorithm>
#include <ipc/notification.hpp>
#include <libk/scope_guard.hpp>
#include <limits>

namespace sched {
auto Budget::info(u64 now) const noexcept -> Info {
    sync::Lock guard{lock_};
    auto balance = balance_;
    u64 refill{};
    for (const auto& r : refills_) {
        if (r.ready <= now) balance += static_cast<__int128>(r.amount);
        else refill += r.amount;
    }
    return {balance, refill, escrow_, generation_,
        !amount_ ? std::numeric_limits<u64>::max() : balance > 0 ? static_cast<u64>(balance) : 0,
        balance < 0 ? static_cast<u64>(-balance) : 0};
}
void Budget::mature(u64 now) noexcept {
    libk_assert(lock_.held());
    while (!refills_.empty() && refills_.front().ready <= now) {
        balance_ += static_cast<__int128>(refills_.front().amount);
        refills_.pop_front();
    }
}
auto Budget::next_refill() const noexcept -> u64 {
    sync::Lock guard{lock_};
    auto balance = balance_;
    for (const auto& r : refills_) {
        balance += static_cast<__int128>(r.amount);
        if (balance > 0) return r.ready;
    }
    return std::numeric_limits<u64>::max(); // Outstanding escrow must really return.
}
auto Budget::reserve(u64 now, u64 limit) noexcept -> Loan {
    sync::Lock guard{lock_};
    mature(now);
    if (!amount_) return {0, generation_};
    const auto amount = balance_ > 0 ? std::min(limit, static_cast<u64>(balance_)) : 0;
    balance_ -= static_cast<__int128>(amount); escrow_ += amount;
    return {amount, generation_};
}
void Budget::record(u64 now, u64 elapsed) noexcept {
    libk_assert(lock_.held());
    mature(now);
    if (!amount_ || !elapsed) return;
    balance_ -= static_cast<__int128>(elapsed);
    // Refill credit itself obeys B/T. Returning all repeated overruns after
    // just T conserves the ledger but silently multiplies actual bandwidth.
    if (refill_tail_ < now) { refill_tail_ = now; refill_fraction_ = 0; }
    const auto scaled = static_cast<unsigned __int128>(elapsed) * period_ + refill_fraction_;
    const auto delay = scaled / amount_;
    refill_fraction_ = static_cast<u64>(scaled % amount_);
    refill_tail_ = delay > std::numeric_limits<u64>::max() - refill_tail_
        ? std::numeric_limits<u64>::max() : refill_tail_ + static_cast<u64>(delay);
    auto ready = std::max(
        libk::checked_add(now, period_).value_or(std::numeric_limits<u64>::max()),
        libk::checked_add(refill_tail_, u64{refill_fraction_ != 0}).value_or(std::numeric_limits<u64>::max()));
    // Different homes may settle samples taken in a different order.
    if (!refills_.empty()) ready = std::max(ready, refills_.back().ready);
    if (!refills_.empty() && (refills_.back().ready == ready || refills_.size() >= capacity_)) {
        auto& last = refills_.back(); last.ready = std::max(last.ready, ready); last.amount += elapsed;
    } else libk_assert(refills_.try_emplace_back(Refill{ready, elapsed}));
}
void Budget::settle(Loan loan, u64 now, u64 elapsed) noexcept {
    sync::Lock guard{lock_};
    libk_assert(loan.generation == generation_ && loan.amount <= escrow_);
    balance_ += static_cast<__int128>(loan.amount); escrow_ -= loan.amount;
    record(now, elapsed);
}
auto Budget::consume(u64 now, u64 elapsed) noexcept -> u64 {
    sync::Lock guard{lock_};
    record(now, elapsed);
    return balance_ < 0 ? static_cast<u64>(-balance_) : 0;
}
auto Budget::configure(u64 amount, u64 period, usize capacity, u64 now) noexcept -> bool {
    sync::Lock guard{lock_};
    if (escrow_ || generation_ == std::numeric_limits<u64>::max()) return false;
    mature(now);
    if (amount != amount_ || period != period_) {
        if (!amount && (balance_ < 0 || !refills_.empty())) return false;
        const auto debt = balance_ < 0 ? static_cast<u64>(-balance_) : 0;
        const auto window = std::max(period, period_);
        const auto scaled = amount ? static_cast<unsigned __int128>(debt) * window : 0;
        const auto delay = window + scaled / (amount ? amount : 1) + (amount && scaled % amount != 0);
        const auto ready = std::max(refill_tail_, delay > std::numeric_limits<u64>::max() - now
            ? std::numeric_limits<u64>::max() : now + static_cast<u64>(delay));
        refill_tail_ = ready; refill_fraction_ = 0;
        refills_.clear(); balance_ = -static_cast<__int128>(debt);
        if (amount) libk_assert(refills_.try_emplace_back(Refill{ready, amount + debt}));
    }
    if (refills_.size() > capacity) {
        const auto ready = refills_.back().ready;
        u64 total{}; for (const auto& r : refills_) total += r.amount;
        refills_.clear(); libk_assert(refills_.try_emplace_back(Refill{ready, total}));
    }
    amount_ = amount; period_ = period; capacity_ = capacity; ++generation_;
    return true;
}

Domain::Local::Local(Domain& owner, CpuId cpu) noexcept
    : owner(owner), cpu(cpu), node{.domain = &owner, .weight = owner.config_.weight},
      refill(Deadline::Fn::bind<&Local::wake>(*this)),
      worker(Env::kernel(*owner.cpus_.get(cpu)->idle().env().kernel_vspace()), {run, this}, &owner) {
    worker.bind_work(*owner.work_, owner.close_work_.key());
}
void Domain::Local::wake() noexcept { current_cpu().dispatcher().domain_ready(*this); }
void Domain::Local::run(void* p) noexcept {
    auto& local = *static_cast<Local*>(p);
    for (;;) {
        if (local.owner.accepting()) (void)local.owner.ordinary_.run(1);
        if (local.owner.ordinary_.arm(local.cpu)) sched::block();
        else sched::yield();
    }
}
Domain::Domain(mm::Pmm& pmm, Cpus& cpus, Config config) noexcept
    : pmm_(pmm), cpus_(cpus), count_(cpus.count()),
      config_(config.cpus ? config : Config{.cpus = count_ >= 64 ? ~u64{} : (u64{1} << count_) - 1}),
      budget_(config_.budget, config_.period),
      locals_(pmm, mm::Slab<Local, false, false>::quota_for(count_)) {
    libk_assert(count_ && valid_config(config_));
}
Domain::~Domain() noexcept {
    libk_assert(!contexts_ && !ops_ && !children_ && !parent_ && !next_parent_ && !pending_children_ && parent_binding_[0].drained() && parent_binding_[1].drained() && !pending_mail_ && !cleanup_);
    if (work_) work_->close(close_work_);
    for (usize cpu = 0; cpu < nodes_.size(); ++cpu) if (nodes_[cpu]) {
        libk_assert(!nodes_[cpu]->active && nodes_[cpu]->node.ready.empty() && !nodes_[cpu]->node.hook.is_linked());
        locals_.destroy(*nodes_[cpu]);
    }
}
auto Domain::open() noexcept -> status_t {
    static_assert(MaxCpus * sizeof(Local*) <= mm::page_size);
    auto fee = resource::acquire(quota_, {.memory = mm::page_size});
    if (!fee) return STATUS_NO_MEMORY;
    auto page = pmm_.allocate_page();
    if (!page) return STATUS_NO_MEMORY;
    index_ = std::move(*page); index_fee_ = std::move(*fee);
    nodes_ = {reinterpret_cast<Local**>(index_.bytes()), count_};
    std::ranges::fill(nodes_, nullptr);
    ordinary_.bind(WorkQueue::Wake::bind<&Domain::wake_work>(*this));
    for (usize cpu = 0; cpu < nodes_.size(); ++cpu) {
        auto made = locals_.create(quota_, *this, CpuId{cpu});
        if (!made) return STATUS_NO_MEMORY;
        nodes_[cpu] = *made;
        libk_assert(nodes_[cpu]->worker.claim_home(cpus_.get(CpuId{cpu})->dispatcher()));
        if (cpu < 64 && (config_.cpus & (u64{1} << cpu))) libk_assert(ordinary_.arm(CpuId{cpu}));
    }
    return STATUS_OK;
}
void Domain::wake_work(CpuId cpu) noexcept {
    if (accepting()) {
        if (!allows(cpu)) {
            const auto config = info(0);
            cpu = CpuId{static_cast<usize>(std::countr_zero(config.cpus))};
        }
        static_cast<void>(local(cpu).worker.execution(true));
        static_cast<void>(sched::wake(cpus_, local(cpu).worker));
    }
    else post_close();
}
auto Domain::post(Work& work) noexcept -> status_t {
    if (!work.cancelable()) return STATUS_BAD_ARGS;
    auto ticket = object::ref<>::retain_alive(work.key());
    std::optional<CpuId> wake;
    { sync::Lock guard{tree_lock_};
      if (!lineage_admitting()) return STATUS_CLOSED;
      wake = ordinary_.enqueue(work, std::move(ticket));
    }
    ordinary_.wake(wake); return STATUS_OK;
}
auto Domain::attach(cap::Resolved<Domain>& parent_cap) noexcept -> status_t {
    auto parent = parent_cap.reference();
    if (!parent) return STATUS_CLOSED;
    const auto& grant = parent_cap.pin();
    return attach(std::move(*parent).as<Domain>().value(), &grant);
}
auto Domain::attach(object::ref<Domain> parent, const cap::GrantPin* grant) noexcept -> status_t {
    auto self = object::ref<>::retain_alive(close_work_.key());
    sync::Lock guard{tree_lock_};
    if (!parent || parent_ || !parent->lineage_open() || phase_ != DOMAIN_OPEN
        || parent->depth() >= max_depth || (config_.cpus & ~parent->config_.cpus)) return STATUS_BAD_ARGS;
    const object::ref<> target{parent};
    const object::ref<>* gates[]{&self, &target};
    auto commit = [&]() noexcept {
        parent_ = std::move(parent); ++tree_version_;
        next_ = parent_->children_; if (next_) next_->prev_ = this;
        parent_->children_ = this;
        for (usize cpu = 0; cpu < nodes_.size(); ++cpu) nodes_[cpu]->node.parent = &parent_->local(CpuId{cpu}).node;
    };
    if (grant) {
        const cap::GrantPin* grants[]{grant};
        return parent_binding_[parent_slot_].attach(grants, gates, cap::Bindings<1>::Action::bind(commit));
    }
    return object::ref<>::publish(gates, commit) ? STATUS_OK : STATUS_CLOSED;
}
auto Domain::depth() const noexcept -> usize {
    libk_assert(tree_lock_.held());
    usize result{};
    for (auto* node = this; node; node = node->parent_ ? &node->parent_.get() : nullptr) ++result;
    return result;
}
auto Domain::lineage_open() const noexcept -> bool {
    libk_assert(tree_lock_.held());
    for (auto* node = this; node; node = node->parent_ ? &node->parent_.get() : nullptr)
        if (!node->accepting()) return false;
    return true;
}
auto Domain::lineage_admitting() const noexcept -> bool {
    libk_assert(tree_lock_.held());
    for (auto* node = this; node; node = node->parent_ ? &node->parent_.get() : nullptr)
        if (node->phase_ == DOMAIN_CLOSING || node->phase_ == DOMAIN_DRAINED
            || !object::ref<>::admitting(node->close_work_.key())) return false;
    return true;
}
void Domain::unstage() noexcept {
    libk_assert(tree_lock_.held() && next_parent_);
    if (next_parent_->closing_pending_ == this) next_parent_->closing_pending_ = pending_next_;
    if (pending_prev_) pending_prev_->pending_next_ = pending_next_;
    else next_parent_->pending_children_ = pending_next_;
    if (pending_next_) pending_next_->pending_prev_ = pending_prev_;
    pending_prev_ = pending_next_ = nullptr;
}
auto Domain::reparent(const cap::Resolved<Domain>& self, cap::Resolved<Domain>& parent_cap) noexcept
    -> std::expected<u64, status_t> {
    auto target = parent_cap.reference();
    auto storage = self.reference();
    auto scan = object::ref<Domain>::retain_alive(close_work_.key());
    if (!target || !storage) return std::unexpected(STATUS_CLOSED);
    auto parent = std::move(*target).as<Domain>().value();
    Source::Publication publication;
    u64 sequence;
    { sync::Lock guard{tree_lock_};
      if (!parent_ || !lineage_open() || pending_mail_ || !parent->lineage_open())
          return std::unexpected(STATUS_BUSY);
      if (&parent.get() == &parent_.get()) return requested_;
      if (config_.cpus & ~parent->config_.cpus) return std::unexpected(STATUS_BAD_ARGS);
      for (auto* node = &parent.get(); node; node = node->parent_ ? &node->parent_.get() : nullptr)
          if (node == this) return std::unexpected(STATUS_BAD_ARGS);
      if (requested_ == std::numeric_limits<u64>::max()) return std::unexpected(STATUS_EXHAUSTED);
      auto& member = parent_binding_[1 - parent_slot_];
      if (!member.reset()) return std::unexpected(STATUS_BUSY);
      const cap::GrantPin* grants[]{&self.pin(), &parent_cap.pin()};
      const object::ref<> destination{parent};
      const object::ref<>* gates[]{&*storage, &destination};
      auto commit = [&]() noexcept {
          next_parent_ = std::move(parent);
          pending_next_ = next_parent_->pending_children_;
          if (pending_next_) pending_next_->pending_prev_ = this;
          next_parent_->pending_children_ = this;
          scan_ = std::move(scan);
          scan_version_ = tree_version_; relocating_ = false;
          config_status_ = STATUS_PENDING; sequence = ++requested_;
          phase_.store(DOMAIN_CONFIGURING, std::memory_order_release);
      };
      const auto status = member.admit([&](auto cells, auto install) noexcept {
          const cap::Graph::Attach item{&parent_cap.pin(), cells[0]};
          return self.pin().graph().admission(grants).attach(std::span{&item, 1}, gates, install)
              ? STATUS_OK : STATUS_CLOSED;
      }, ParentBinding::Action::bind(commit));
      if (status != STATUS_OK) return std::unexpected(status);
      publication = events_.prepare(DOMAIN_CONFIGURING, true);
    }
    if (publication) object::changed(close_work_.key());
    publication.publish(); notify(true); post_close(); return sequence;
}
auto Domain::finish_move() noexcept -> bool {
    std::array<object::ref<Domain>, 10> released;
    usize releases{};
    // The proposed parent has its own real membership until either commit or
    // cancellation; close of either parent can therefore find this child.
    { sync::Lock guard{tree_lock_};
      if (phase_ != DOMAIN_CONFIGURING || !next_parent_) return false;
      if (pending_mail_) return true;
      for (usize cpu = 0; cpu < nodes_.size(); ++cpu) if (!nodes_[cpu]->stopped) return true;
      if (scan_version_ != tree_version_) {
          released[releases++] = std::move(scan_);
          scan_ = object::ref<Domain>::retain_alive(close_work_.key());
          scan_version_ = tree_version_;
      }
      for (usize step = 0; scan_ && step < 8; ++step) {
          auto* node = &scan_.get();
          if (node->depth() - depth() + 1 + next_parent_->depth() > max_depth) {
              config_status_ = STATUS_BAD_ARGS; released[releases++] = std::move(scan_); break;
          }
          Domain* next = node->children_;
          if (!next) {
              while (node != this && !node->next_) node = &node->parent_.get();
              if (node != this) next = node->next_;
          }
          released[releases++] = std::move(scan_);
          scan_ = next ? object::ref<Domain>::retain_alive(next->close_work_.key()) : object::ref<Domain>{};
      }
      if (scan_) { post_close(); return true; }
      if (config_status_ == STATUS_PENDING && !relocating_) {
          relocating_ = true; notify(true); return true;
      }
      if (config_status_ != STATUS_PENDING && relocating_) {
          guard.restore(); static_cast<void>(close()); return true;
      }
    }
    bool failed;
    { sync::Lock guard{tree_lock_}; failed = config_status_ != STATUS_PENDING; }
    if (!parent_binding_[failed ? 1 - parent_slot_ : parent_slot_].close()) return true;
    Source::Publication publication;
    object::ref<Domain> old;
    bool changed{};
    { sync::Lock guard{tree_lock_};
      if (phase_ != DOMAIN_CONFIGURING) return true;
      if (!failed && scan_version_ != tree_version_) { post_close(); return true; }
      if (!next_parent_->lineage_open() || parent_binding_[1 - parent_slot_].revoked()) {
          // Old membership may already have drained. Do not reopen an
          // execution domain without a valid parent authorization.
          guard.restore(); static_cast<void>(close()); return true;
      }
      unstage();
      if (failed) old = std::move(next_parent_);
      else {
          old = std::move(parent_);
          if (old->closing_child_ == this) old->closing_child_ = next_;
          if (prev_) prev_->next_ = next_; else old->children_ = next_;
          if (next_) next_->prev_ = prev_;
          parent_ = std::move(next_parent_); parent_slot_ = 1 - parent_slot_;
          prev_ = nullptr; next_ = parent_->children_;
          if (next_) next_->prev_ = this;
          parent_->children_ = this;
          ++tree_version_; config_status_ = STATUS_OK;
      }
      relocating_ = false; applied_ = requested_; phase_.store(DOMAIN_OPEN, std::memory_order_release);
      publication = events_.prepare(0, true); changed = true;
    }
    if (old) old->post_close();
    if (publication) object::changed(close_work_.key());
    publication.publish();
    if (changed) notify(true);
    return true;
}
auto Domain::info(u64 now) const noexcept -> DomainInfo {
    sync::Lock guard{tree_lock_};
    const auto b = budget_.info(now);
    return {.version=2, .size=sizeof(DomainInfo), .cpus=config_.cpus, .budget=config_.budget,
        .period=config_.period, .available=b.available, .debt=b.debt, .escrow=b.escrow,
        .weight=config_.weight, .phase=phase_.load(), .requested=requested_, .applied=applied_, .runtime=runtime_.load(), .work_runtime=work_runtime_.load(), .status=config_status_, .reserved=0};
}
void Domain::notify(bool all) noexcept {
    for (usize cpu = 0; cpu < nodes_.size(); ++cpu) if (nodes_[cpu] && (all || nodes_[cpu]->waiting.exchange(false, std::memory_order_acq_rel)))
        cpus_.get(CpuId{cpu})->dispatcher().post(*nodes_[cpu]);
}
auto Domain::close() noexcept -> u32 {
    Source::Publication publication;
    bool changed{};
    { sync::Lock guard{tree_lock_};
      if (phase_ == DOMAIN_OPEN || phase_ == DOMAIN_CONFIGURING) {
          phase_.store(DOMAIN_CLOSING, std::memory_order_release);
          closing_child_ = children_; closing_pending_ = pending_children_; closing_sc_ = contexts_; closing_op_ = ops_;
          if (pending_config_ || next_parent_) { pending_config_.reset(); applied_ = requested_; config_status_ = STATUS_CANCELED; }
          publication = events_.prepare(DOMAIN_CLOSING, true);
          changed = true;
      } }
    if (publication) object::changed(close_work_.key());
    publication.publish();
    if (changed) notify(true);
    post_close(); return phase_.load();
}
auto Domain::configure(Config config, Domain* parent) noexcept -> std::expected<u64, status_t> {
    u64 sequence;
    Source::Publication publication;
    { sync::Lock guard{tree_lock_};
      if (!valid_config(config)) return std::unexpected(STATUS_BAD_ARGS);
      if (!lineage_open() || pending_mail_) return std::unexpected(STATUS_BUSY);
      if (requested_ == std::numeric_limits<u64>::max()) return std::unexpected(STATUS_EXHAUSTED);
      if (parent_ && (config.cpus & ~parent_->config_.cpus)) return std::unexpected(STATUS_BAD_ARGS);
      const bool increase = config.weight > config_.weight || (config.cpus & ~config_.cpus)
          || (config_.budget && (!config.budget || config.budget > config_.budget || config.period < config_.period));
      if (increase && parent_ && parent != &parent_.get()) return std::unexpected(STATUS_DENIED);
      for (auto* child = children_; child; child = child->next_)
          if (child->config_.cpus & ~config.cpus) return std::unexpected(STATUS_BUSY);
      // CPU removal follows actual SC migration/unbinding, never implicit
      // takeover of a possibly still executing home.
      for (auto* sc = contexts_; sc; sc = sc->next_)
          if (sc->config_.cpus & ~config.cpus) return std::unexpected(STATUS_BUSY);
      sequence = ++requested_; pending_config_ = config; config_status_ = STATUS_PENDING;
      phase_.store(DOMAIN_CONFIGURING, std::memory_order_release);
      publication = events_.prepare(DOMAIN_CONFIGURING, true);
    }
    if (publication) object::changed(close_work_.key());
    publication.publish();
    notify(true); post_close(); return sequence;
}
void Domain::retire(object::cleanup&& cleanup) noexcept {
    { sync::Lock guard{tree_lock_}; cleanup_ = std::move(cleanup); }
    static_cast<void>(close());
}
void Domain::post_close() noexcept { if (work_) work_->post(close_work_); }
void Domain::finish_close() noexcept {
    Source::Publication publication;
    bool configured{};
    { sync::Lock guard{tree_lock_};
      if (phase_ == DOMAIN_CONFIGURING && pending_config_) {
          if (pending_mail_) return;
          for (usize cpu = 0; cpu < nodes_.size(); ++cpu) if (nodes_[cpu] && !nodes_[cpu]->stopped) return;
          libk_assert(pending_config_);
          const auto config = *pending_config_;
          config_status_ = budget_.configure(config.budget, config.period, Budget::max_refills,
              current_cpu().dispatcher().clock().rd_ticks()) ? STATUS_OK : STATUS_BUSY;
          if (config_status_ == STATUS_OK) config_ = config;
          pending_config_.reset(); applied_ = requested_;
          phase_.store(DOMAIN_OPEN, std::memory_order_release);
          publication = events_.prepare(0, true); configured = true;
      }
    }
    if (configured) {
        if (publication) object::changed(close_work_.key());
    publication.publish(); notify(true); return;
    }
    if (finish_move()) return;
    bool closing;
    { sync::Lock guard{tree_lock_}; closing = phase_ == DOMAIN_CLOSING || phase_ == DOMAIN_DRAINED; }
    if (!closing) return;
    if (ordinary_.run(8, true)) { post_close(); return; }
    for (usize cpu = 0; cpu < nodes_.size(); ++cpu) if (nodes_[cpu] && !nodes_[cpu]->worker.stop_requested())
        nodes_[cpu]->worker.stop();
    // Initiate every real child relation in finite batches; do not wait for one
    // unresponsive home before requesting the others to drain.
    for (usize step = 0; step < 8; ++step) {
        object::ref<Sc> sc;
        object::ref<Domain> child;
        bool canceled{};
        { sync::Lock guard{tree_lock_};
          if (phase_ != DOMAIN_CLOSING && phase_ != DOMAIN_DRAINED) return;
          if (closing_op_) {
              auto& op = *closing_op_; closing_op_ = op.cpu_next_;
              // The index lock keeps an inline control record from being
              // reused. Only publish prepaid intent; no provider executes here.
              if (op.cancel() == STATUS_TOO_LATE) op.wake();
              canceled = true;
          } else if (closing_child_) {
              child = object::ref<Domain>::retain_alive(closing_child_->close_work_.key());
              closing_child_ = closing_child_->next_;
          } else if (closing_pending_) {
              child = object::ref<Domain>::retain_alive(closing_pending_->close_work_.key());
              closing_pending_ = closing_pending_->pending_next_;
          } else if (closing_sc_) {
              sc = object::ref<Sc>::retain_alive(closing_sc_->close_work_.key());
              closing_sc_ = closing_sc_->next_;
          }
        }
        if (child) static_cast<void>(child->close());
        else if (sc) {
            { sync::Lock guard{sc->lock_}; sc->withdrawing_ = true; }
            sc->post_close();
        } else if (!canceled) break;
        if (step == 7) { post_close(); return; }
    }
    { sync::Lock guard{tree_lock_};
      if ((phase_ != DOMAIN_CLOSING && phase_ != DOMAIN_DRAINED) || contexts_ || ops_ || children_ || pending_children_ || pending_mail_) return;
      for (usize cpu = 0; cpu < nodes_.size(); ++cpu) if (nodes_[cpu] && (!nodes_[cpu]->stopped || !nodes_[cpu]->worker.stopped())) return;
      if (!ordinary_.drained()) return;
    }
    if (!parent_binding_[0].close() || !parent_binding_[1].close()) return;
    object::ref<Domain> staged, scan;
    { sync::Lock guard{tree_lock_};
      scan = std::move(scan_);
      if (next_parent_) { unstage(); staged = std::move(next_parent_); }
    }
    if (staged) staged->post_close();
    object::ref<Domain> parent;
    { sync::Lock guard{tree_lock_};
      if (parent_) {
          parent = std::move(parent_); ++tree_version_;
          if (parent->closing_child_ == this) parent->closing_child_ = next_;
          for (usize cpu = 0; cpu < nodes_.size(); ++cpu) if (nodes_[cpu]) nodes_[cpu]->node.parent = nullptr;
          if (prev_) prev_->next_ = next_; else parent->children_ = next_;
          if (next_) next_->prev_ = prev_;
          prev_ = next_ = nullptr;
      }
    }
    if (parent) parent->post_close();
    { sync::Lock guard{tree_lock_};
      if (phase_.exchange(DOMAIN_DRAINED, std::memory_order_acq_rel) != DOMAIN_DRAINED)
          publication = events_.prepare(DOMAIN_DRAINED, true);
    }
    if (publication) object::changed(close_work_.key());
    publication.publish();
    bool retiring;
    { sync::Lock guard{tree_lock_}; retiring = bool(cleanup_); }
    if (retiring) { events_.close(); if (!events_.drained()) return; }
    object::cleanup done;
    { sync::Lock guard{tree_lock_}; done = std::move(cleanup_); }
    if (done) { work_->close(close_work_); done.complete(); }
}
auto Domain::admit(Sc& sc, CpuId cpu) noexcept -> Result {
    sync::Lock guard{tree_lock_}; return admit_locked(sc, cpu);
}
auto Domain::admit_locked(Sc& sc, CpuId cpu) noexcept -> Result {
    libk_assert(tree_lock_.held());
    if (!lineage_open()) return std::unexpected(Error::Busy);
    if (!allows_locked(cpu) || !(sc.config_.cpus & (u64{1} << cpu.raw))) return std::unexpected(Error::InvalidCpu);
    sc.config_.cpus &= config_.cpus;
    if (sc.domain_) return std::unexpected(Error::Busy);
    sc.domain_ = this;
    sc.next_ = contexts_; if (contexts_) contexts_->prev_ = &sc;
    contexts_ = &sc;
    return {};
}
auto Domain::unadmit(Sc& sc) noexcept -> Result {
    { sync::Lock guard{tree_lock_};
      if (sc.domain_ != this) return std::unexpected(Error::NotAdmitted);
      if (sc.owner_) return std::unexpected(Error::Busy);
      if (closing_sc_ == &sc) closing_sc_ = sc.next_;
      if (sc.prev_) sc.prev_->next_ = sc.next_; else contexts_ = sc.next_;
      if (sc.next_) sc.next_->prev_ = sc.prev_;
      sc.prev_ = sc.next_ = nullptr; sc.domain_ = nullptr;
    }
    post_close(); return {};
}


void Sc::domain_revoked() noexcept {
    { sync::Lock guard{lock_}; withdrawing_ = true; }
    post_close();
}
Sc::Sc(Config config, u64 now) noexcept
    : config_(config), budget_(config.budget, config.period, config.refill_capacity) {
    libk_assert(valid_config(config));
    static_cast<void>(now);
}
Sc::~Sc() noexcept {
    libk_assert(!owner_ && !cleanup_ && !domain_);
    libk_assert(domain_binding_.drained());
    if (work_) work_->close(close_work_);
}
auto Sc::available(u64 now) const noexcept -> u64 { return budget_.info(now).available; }
auto Sc::next_refill() const noexcept -> u64 { return budget_.next_refill(); }
void Sc::record(u64 elapsed) noexcept {
    sync::Lock guard{lock_};
    runtime_ += elapsed;
    const auto scaled = static_cast<unsigned __int128>(elapsed) * 1024 + fraction_;
    service_ += static_cast<u64>(scaled / config_.weight);
    fraction_ = static_cast<u64>(scaled % config_.weight);
}
auto Sc::charge(u64 now, u64 elapsed) noexcept -> u64 {
    record(elapsed);
    return budget_.consume(now, elapsed);
}
void Sc::locate(Dispatcher& d) noexcept {
    sync::Lock guard{lock_};
    const auto floor = domain_->local(d.id()).node.floor;
    if (location_ && location_ != &d) service_ = floor + lag_;
    else service_ = std::max(service_, floor);
    location_ = &d;
}
auto Sc::info(u64 now) const noexcept -> ScInfo {
    sync::Lock guard{lock_};
    const auto budget = budget_.info(now);
    return {.version = 1, .size = sizeof(ScInfo), .generation = generation_, .applied = applied_,
        .cpus = config_.cpus, .runtime = runtime_, .budget = config_.budget, .period = config_.period,
        .available = budget.available, .debt = budget.debt, .weight = config_.weight, .bound = owner_ != nullptr};
}
auto Sc::configure(Config config, u64 now) noexcept -> Result {
    bool changed{};
    libk::scope_exit notify{[&]() noexcept { if (changed) state_changed(); }};
    sync::Lock guard{lock_};
    if (!valid_config(config) || !domain_ || withdrawing_) return std::unexpected(Error::InvalidConfig);
    sync::Lock topology{Domain::tree_lock_};
    if (domain_->phase_ != DOMAIN_OPEN) return std::unexpected(Error::Active);
    if (owner_) return std::unexpected(Error::Active);
    for (usize cpu = 0; cpu < 64; ++cpu)
        if ((config.cpus & (u64{1} << cpu)) && !domain_->allows_locked(CpuId{cpu}))
            return std::unexpected(Error::WrongCpu);
    if (!budget_.configure(config.budget, config.period, config.refill_capacity, now))
        return std::unexpected(Error::Active);
    fraction_ = fraction_ * config.weight / config_.weight;
    config_ = config;
    changed = true;
    return {};
}
void Sc::state_changed() noexcept {
    Source::Publication publication;
    bool changed;
    {
        sync::Lock guard{lock_};
        const u64 bits = (owner_ ? SC_BOUND : 0U) | (withdrawing_ ? SC_WITHDRAWING : 0U);
        changed = events_.snapshot().value != bits;
        publication = events_.prepare(bits, true);
    }
    if (changed) object::changed(close_work_.key());
}
auto Sc::admit(cap::Resolved<Domain>& parent_cap, CpuId cpu) noexcept -> Result {
    auto target = parent_cap.reference();
    auto self = object::ref<>::retain_alive(close_work_.key());
    if (!target) return std::unexpected(Error::NotAdmitted);
    sync::Lock guard{lock_};
    sync::Lock topology{Domain::tree_lock_};
    auto& domain = parent_cap.object();
    if (domain_ || withdrawing_ || !domain.lineage_open()) return std::unexpected(Error::AlreadyAdmitted);
    if (!domain.allows_locked(cpu) || !(config_.cpus & (u64{1} << cpu.raw))) return std::unexpected(Error::WrongCpu);
    const object::ref<>* gates[]{&self, &*target};
    const auto& grant = parent_cap.pin();
    const cap::GrantPin* grants[]{&grant};
    auto commit = [&]() noexcept { libk_assert(domain.admit_locked(*this, cpu)); };
    if (domain_binding_.attach(grants, gates, cap::Bindings<1>::Action::bind(commit)) != STATUS_OK)
        return std::unexpected(Error::NotAdmitted);
    return {};
}
auto Sc::bind_locked(object::ref<Sc> self, Thread& target) noexcept -> Result {
    if (!self || &self.get() != this || !domain_) return std::unexpected(Error::NotAdmitted);
    if (withdrawing_ || owner_ || generation_ == std::numeric_limits<u64>::max()) return std::unexpected(Error::AlreadyBound);
    if (!target.can_bind(*this)) return std::unexpected(Error::AlreadyBound);
    target.bind(std::move(self), generation_ + 1);
    owner_ = &target;
    ++generation_;
    return {};
}
auto Sc::bind(object::ref<Sc> self, Thread& target) noexcept -> Result {
    Result result = std::unexpected(Error::Active);
    Dispatcher* home{};
    { sync::Lock guard{lock_};
      sync::Lock topology{Domain::tree_lock_};
      sync::Lock thread{target.lock_};
      if (!domain_ || owner_ || withdrawing_ || releasing_ || !binding_.reset()) return result;
      auto domain = object::ref<>::retain_alive(domain_->close_work_.key());
      auto beneficiary = object::ref<>::retain_alive(target.finalizer_.key());
      const object::ref<> execution{self};
      const object::ref<>* gates[]{&domain, &beneficiary, &execution};
      static_cast<void>(object::ref<>::publish(gates, [&]() noexcept {
          result = bind_locked(std::move(self), target); home = target.home_.load();
      }));
    }
    if (result) home->request_control(target);
    return result;
}
auto Sc::bind(object::ref<Sc> self, Thread& target,
    const cap::Resolved<Sc>& context, const cap::Resolved<Thread>& thread) noexcept -> Result {
    Dispatcher* home;
    { sync::Lock guard{lock_};
      sync::Lock topology{Domain::tree_lock_};
      sync::Lock beneficiary{target.lock_};
      if (withdrawing_ || owner_ || releasing_ || generation_ == std::numeric_limits<u64>::max()
          || !target.can_bind(*this) || !binding_.reset()) return std::unexpected(Error::AlreadyBound);
      auto domain = object::ref<>::retain_alive(domain_->close_work_.key());
      const object::ref<>* gates[]{&domain};
      const cap::GrantPin* grants[]{&context.pin(), &thread.pin()};
      auto commit = [&]() noexcept { libk_assert(bind_locked(std::move(self), target)); };
      if (binding_.attach(grants, gates, cap::Bindings<2>::Action::bind(commit)) != STATUS_OK)
          return std::unexpected(Error::Active);
      home = target.home_;
    }
    home->request_control(target); return {};
}
auto Sc::withdraw(u64 generation) noexcept -> Result {
    sync::Lock guard{lock_};
    if (!owner_) return std::unexpected(Error::NotBound);
    if (generation != generation_) return std::unexpected(Error::Stale);
    sync::Lock target_guard{owner_->lock_};
    owner_->detach_sc_ = true;
    owner_->home_.load(std::memory_order_acquire)->post(*owner_, Dispatcher::Control);
    return owner_->home_.load(std::memory_order_acquire)->kick_remote() ? Result{} : Result{std::unexpected(Error::Active)};
}
void Sc::release(Thread& target) noexcept {
    { sync::Lock guard{lock_};
      libk_assert(owner_ == &target);
      owner_ = nullptr; applied_ = 0;
      if (location_) lag_ = service_ > domain_->local(location_->id()).node.floor
          ? service_ - domain_->local(location_->id()).node.floor : 0;
    }
    state_changed(); static_cast<void>(drain_binding()); post_close();
}
auto Sc::drain_binding() noexcept -> bool {
    { sync::Lock guard{lock_};
      if (owner_ || releasing_) return false;
      if (binding_.drained()) return true;
      releasing_ = true;
    }
    const bool drained = binding_.close();
    { sync::Lock guard{lock_}; releasing_ = false; }
    return drained;
}
void Sc::retire(object::cleanup&& cleanup) noexcept {
    { sync::Lock guard{lock_}; cleanup_ = std::move(cleanup); withdrawing_ = true; }
    post_close();
}
void Sc::post_close() noexcept { if (work_) work_->post(close_work_); }
void Sc::finish_close() noexcept {
    bool withdraw{};
    {
        sync::Lock guard{lock_};
        withdraw = binding_.revoked() || withdrawing_;
        if (owner_) {
            if (withdraw) {
                sync::Lock target_guard{owner_->lock_};
                owner_->detach_sc_ = true;
                owner_->home_.load(std::memory_order_acquire)->post(*owner_, Dispatcher::Control);
                static_cast<void>(owner_->home_.load(std::memory_order_acquire)->kick_remote());
            }
            return;
        }
    }
    if (!drain_binding()) return;
    { sync::Lock guard{lock_}; if (owner_ || !withdrawing_) return; }
    if (!domain_binding_.close()) return;
    { sync::Lock guard{lock_}; if (domain_) libk_assert(domain_->unadmit(*this)); }
    bool retiring;
    { sync::Lock guard{lock_}; retiring = bool(cleanup_); }
    if (retiring) { events_.close(); if (!events_.drained()) return; }
    object::cleanup done;
    { sync::Lock guard{lock_}; done = std::move(cleanup_); }
    if (done) { work_->close(close_work_); done.complete(); }
}

} // namespace sched

// Accepted work keeps its original CPU quota independently of the submitting
// Thread/SC. Both the provider and this Domain link share final Gate admission.
auto Op::admit_cpu(Publish publish) noexcept -> status_t {
    auto domain = cpu_.as<sched::Domain>();
    if (!domain) return STATUS_BAD_ARGS;
    sync::Lock guard{sched::Domain::tree_lock_};
    if (!domain->get().lineage_admitting()) return STATUS_CLOSED;
    const auto status = publish();
    if (status != STATUS_OK) return status;
    auto& owner = domain->get();
    cpu_next_ = owner.ops_;
    if (cpu_next_) cpu_next_->cpu_previous_ = this;
    owner.ops_ = this; cpu_admitted_ = true;
    return STATUS_OK;
}
auto Op::schedule() noexcept -> bool {
    object::ref<> cpu;
    { sync::Lock guard{lock_}; cpu = cpu_; }
    auto domain = cpu.as<sched::Domain>();
    if (!domain) return false;
    auto ticket = object::ref<>::retain_alive(runner_->key());
    std::optional<CpuId> wake;
    auto& owner = domain->get();
    {
        sync::Lock topology{sched::Domain::tree_lock_};
        sync::Lock guard{lock_};
        if (cpu_.id() != cpu.id() || phase_ == OP_DONE || phase_ == OP_DRAINING || safety_ || canceled_
            || !owner.lineage_admitting()) return false;
        wake = owner.ordinary_.enqueue(*runner_, std::move(ticket));
    }
    owner.ordinary_.wake(wake);
    return true;
}
void Op::run() noexcept {
    object::ref<> provider;
    Source::Action progress;
    { sync::Lock guard{lock_}; provider = provider_; progress = progress_; }
    if (progress) progress();
}
void Op::cancel_work() noexcept {
    object::ref<> cpu, ticket;
    { sync::Lock guard{lock_}; cpu = cpu_; }
    if (auto domain = cpu.as<sched::Domain>()) {
        sync::Lock topology{sched::Domain::tree_lock_};
        sync::Lock guard{lock_};
        if (cpu_.id() == cpu.id()) {
            safety_ = true;
            if (phase_ == OP_ACCEPTED) canceled_ = true;
            ticket = domain->get().ordinary_.close(*runner_, runner_ == &job_);
        }
    }
    wake(); // Committed work continues only its safety tail on maintenance.
}
void Op::release_cpu() noexcept {
    object::ref<> cpu, released, ticket;
    { sync::Lock guard{lock_}; cpu = cpu_; }
    auto domain = cpu.as<sched::Domain>();
    if (!domain) return;
    auto& owner = domain->get();
    {
        sync::Lock topology{sched::Domain::tree_lock_};
        sync::Lock guard{lock_};
        released = std::move(cpu_); progress_ = {};
        ticket = owner.ordinary_.close(*runner_, runner_ == &job_);
        if (cpu_admitted_) {
            if (owner.closing_op_ == this) owner.closing_op_ = cpu_next_;
            if (cpu_previous_) cpu_previous_->cpu_next_ = cpu_next_; else owner.ops_ = cpu_next_;
            if (cpu_next_) cpu_next_->cpu_previous_ = cpu_previous_;
            cpu_previous_ = cpu_next_ = nullptr; cpu_admitted_ = false;
        }
    }
    owner.post_close();
}

auto Op::commit() noexcept -> bool {
    object::ref<> cpu;
    { sync::Lock guard{lock_}; cpu = cpu_; }
    auto domain = cpu.as<sched::Domain>();
    std::optional<sync::Lock<sync::Spin>> topology;
    if (domain) topology.emplace(sched::Domain::tree_lock_);
    bool committed{};
    auto publish = [&]() noexcept {
        sync::Lock guard{lock_};
        libk_assert(phase_ == OP_ACCEPTED);
        if (canceled_ || safety_ || (domain && !domain->get().lineage_admitting())) return;
        phase_ = OP_COMMITTED; committed = true;
    };
    (void)bindings_.publish(Source::Action::bind(publish));
    return committed;
}

auto Op::start(object::ref<> cpu, const object::ref<>& self, cap::Graph::Admission admission,
    std::span<const object::ref<>*> gates, Source::Action install, const cap::GrantPin* provider_grant) noexcept -> status_t {
    cpu_domain(std::move(cpu));
    auto admitted = [&]() noexcept {
        auto commit = [&]() noexcept { accept(); if (install) install(); };
        return bindings_.admit([&](std::span<cap::Attachment*> cells, Source::Action publish) noexcept {
            const cap::Graph::Attach items[]{{provider_grant, cells[0], provider_grant ? nullptr : &self},
                {nullptr, cells[1], &cpu_}};
            return admission.attach(items, gates, publish) ? STATUS_OK : STATUS_CLOSED;
        }, Source::Action::bind(commit));
    };
    return admit_cpu(Publish::bind(admitted));
}
