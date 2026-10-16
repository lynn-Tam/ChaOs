#include <sched/sc.hpp>
#include <sched/dispatcher.hpp>
#include <cpu/cpu.hpp>
#include <algorithm>
#include <ipc/notification.hpp>
#include <libk/scope_guard.hpp>
#include <limits>

namespace sched {
auto Domain::admit(Sc& sc, CpuId cpu) noexcept -> Result {
    if (!allows(cpu) || !(sc.config_.cpus & (u64{1} << cpu.raw))) return std::unexpected(Error::InvalidCpu);
    sc.config_.cpus &= count_ == 64 ? ~u64{0} : (u64{1} << count_) - 1;
    if (sc.domain_) return std::unexpected(Error::Busy);
    sc.domain_ = this;
    return {};
}
auto Domain::unadmit(Sc& sc) noexcept -> Result {
    if (sc.domain_ != this) return std::unexpected(Error::NotAdmitted);
    if (sc.owner_) return std::unexpected(Error::Busy);
    sc.domain_ = nullptr;
    return {};
}


const cap::GrantAttachmentOps Sc::auth::ops_{
    .invalidate = [](void* p, cap::GrantWork&& work) noexcept {
        auto& cell = *static_cast<link*>(p);
        cell.owner->invalidate(cell, std::move(work));
    },
    .released = [](void* p) noexcept { static_cast<link*>(p)->owner->sc_->post_close(); },
};
Sc::auth::auth(Sc& sc) noexcept : sc_(&sc), context_(*this), target_cap_(*this) {}
Sc::auth::~auth() noexcept { release(); libk_assert(reusable()); }

auto Sc::auth::attach(const cap::Resolved<Sc>& context, const cap::Resolved<Thread>& target) noexcept
    -> std::expected<void, cap::GrantError> {
    flight publishing{*this};
    {
        sync::Lock guard{lock_};
        if (ended_ || attaching_ || revoking_) return std::unexpected(cap::GrantError::InvalidState);
        attaching_ = true;
    }
    auto result = context.attach(context_.attachment);
    if (result) result = target.attach(target_cap_.attachment);
    bool accepted;
    {
        sync::Lock guard{lock_};
        attaching_ = false;
        accepted = result && !ended_ && !revoking_;
    }
    if (!accepted) release();
    if (!result) return result;
    return accepted ? std::expected<void, cap::GrantError>{}
                    : std::unexpected(cap::GrantError::InvalidState);
}

auto Sc::auth::reusable() const noexcept -> bool {
    sync::Lock guard{lock_};
    return ended_ && !attaching_ && !publishers_
        && !context_.attachment.busy() && !target_cap_.attachment.busy();
}

void Sc::auth::invalidate(link& cell, cap::GrantWork&& work) noexcept {
    flight publishing{*this};
    {
        sync::Lock guard{lock_};
        libk_assert(!cell.work);
        cell.work = std::move(work);
        revoking_ = true;
    }
    sc_->post_close();
}
void Sc::auth::release() noexcept {
    {
        sync::Lock guard{lock_};
        if (ended_ && context_.detaching && target_cap_.detaching) return;
    }
    flight publishing{*this};
    {
        sync::Lock guard{lock_};
        ended_ = true;
        if (attaching_) return;
    }
    for (auto* cell : {&context_, &target_cap_}) {
        bool detach;
        {
            sync::Lock guard{lock_};
            detach = !std::exchange(cell->detaching, true);
        }
        if (detach && cell->attachment.attached()) static_cast<void>(cell->attachment.detach());
        drain(*cell);
    }
}
void Sc::auth::drain(link& cell) noexcept {
    cap::GrantWork work;
    { sync::Lock guard{lock_}; work = std::move(cell.work); }
    work.reset();
}

const cap::GrantAttachmentOps Sc::domain_ops_{
    .invalidate = [](void* p, cap::GrantWork&& work) noexcept {
        auto& sc = *static_cast<Sc*>(p);
        {
            sync::Lock guard{sc.lock_};
            sc.domain_work_ = std::move(work);
            sc.withdrawing_ = true;
        }
        sc.post_close();
    },
    .released = [](void* p) noexcept { static_cast<Sc*>(p)->post_close(); },
};
Sc::Sc(Config config, u64 now) noexcept : config_(config) {
    libk_assert(valid_config(config));
    if (config.budget) libk_assert(refills_.try_emplace_back(Refill{now, config.budget}));
}
Sc::~Sc() noexcept {
    changed_.reset();
    libk_assert(!owner_ && !auth_ && !cleanup_ && !domain_);
    libk_assert(!domain_cap_.busy() && !domain_work_);
    if (work_) work_->close(close_work_);
}
auto Sc::available(u64 now) const noexcept -> u64 {
    sync::Lock guard{lock_};
    if (!config_.budget) return std::numeric_limits<u64>::max();
    u64 total{};
    for (const auto& r : refills_) { if (r.ready_at > now) break; total += r.amount; }
    return total > debt_ ? total - debt_ : 0;
}
auto Sc::next_refill() const noexcept -> u64 {
    sync::Lock guard{lock_};
    u64 total{};
    for (const auto& r : refills_) {
        total += r.amount;
        if (total > debt_) return r.ready_at;
    }
    // Debt can exceed one window after an unpreemptible kernel section.
    // Repay it at each real refill; never synthesize available budget.
    return refills_.back().ready_at;
}
auto Sc::charge(u64 now, u64 elapsed) noexcept -> u64 {
    sync::Lock guard{lock_};
    runtime_ += elapsed;
    const auto scaled = static_cast<unsigned __int128>(elapsed) * 1024 + fraction_;
    service_ += static_cast<u64>(scaled / config_.weight);
    fraction_ = static_cast<u64>(scaled % config_.weight);
    if (!config_.budget) return 0;
    u64 remaining = elapsed + std::exchange(debt_, 0), consumed{};
    while (remaining && !refills_.empty() && refills_.front().ready_at <= now) {
        auto& r = refills_.front();
        const u64 n = std::min(remaining, r.amount);
        remaining -= n; consumed += n; r.amount -= n;
        if (!r.amount) refills_.pop_front();
    }
    debt_ = remaining;
    if (consumed) {
        const u64 ready = now + config_.period;
        if (!refills_.empty() && (refills_.back().ready_at == ready
            || refills_.size() == config_.refill_capacity)) {
            auto& r = refills_.back(); r.ready_at = ready; r.amount += consumed;
        } else libk_assert(refills_.try_emplace_back(Refill{ready, consumed}));
    }
    return remaining;
}
void Sc::locate(Dispatcher& d) noexcept {
    sync::Lock guard{lock_};
    if (location_ && location_ != &d) {
        service_ = d.fair_.load() + lag_;
    } else service_ = std::max(service_, d.fair_.load());
    location_ = &d;
}
auto Sc::info(u64 now) const noexcept -> ScInfo {
    sync::Lock guard{lock_};
    u64 available = std::numeric_limits<u64>::max();
    if (config_.budget) {
        available = 0;
        for (const auto& r : refills_) { if (r.ready_at > now) break; available += r.amount; }
        available = available > debt_ ? available - debt_ : 0;
    }
    return {.version = 1, .size = sizeof(ScInfo), .generation = generation_, .applied = applied_,
        .cpus = config_.cpus, .runtime = runtime_, .budget = config_.budget, .period = config_.period,
        .available = available, .debt = debt_, .weight = config_.weight, .bound = owner_ != nullptr};
}
auto Sc::configure(Config config, u64 now) noexcept -> Result {
    bool changed{};
    libk::scope_exit notify{[&]() noexcept { if (changed) (void)changed_.signal(); }};
    sync::Lock guard{lock_};
    if (!valid_config(config) || !domain_ || withdrawing_) return std::unexpected(Error::InvalidConfig);
    if (owner_ || attaching_) return std::unexpected(Error::Active);
    for (usize cpu = 0; cpu < 64; ++cpu)
        if ((config.cpus & (u64{1} << cpu)) && !domain_->allows(CpuId{cpu}))
            return std::unexpected(Error::WrongCpu);
    if (config.budget != config_.budget || config.period != config_.period) {
        // Removing the cap requires its previous window and debt to have drained.
        if (!config.budget && (debt_ || refills_.back().ready_at > now)) return std::unexpected(Error::Active);
        const u64 wait = std::max(config.period, config_.period);
        refills_.clear();
        if (config.budget) libk_assert(refills_.try_emplace_back(Refill{now + wait, config.budget}));
    }
    if (refills_.size() > config.refill_capacity) {
        const auto ready = refills_.back().ready_at;
        u64 amount{}; for (const auto& refill : refills_) amount += refill.amount;
        refills_.clear(); libk_assert(refills_.try_emplace_back(Refill{ready, amount}));
    }
    fraction_ = fraction_ * config.weight / config_.weight;
    config_ = config;
    changed = true;
    return {};
}
auto Sc::observe(ipc::Notification& notification, u64 badge) noexcept -> bool {
    {
        sync::Lock guard{lock_};
        if (!notification.bind(changed_, badge)) return false;
    }
    static_cast<void>(changed_.signal());
    return true;
}
auto Sc::admit(cap::Resolved<Domain>& authority, CpuId cpu) noexcept -> Result {
    sync::Lock guard{lock_};
    if (domain_ || withdrawing_) return std::unexpected(Error::AlreadyAdmitted);
    if (!authority.attach(domain_cap_)) return std::unexpected(Error::InvalidConfig);
    if (!authority->admit(*this, cpu)) {
        static_cast<void>(domain_cap_.detach());
        return std::unexpected(Error::WrongCpu);
    }
    return {};
}
auto Sc::bind_locked(object::ref<Sc> self, Thread& target) noexcept -> Result {
    if (!self || &self.get() != this || !domain_) return std::unexpected(Error::NotAdmitted);
    if (withdrawing_ || owner_) return std::unexpected(Error::AlreadyBound);
    if (!target.try_bind(std::move(self), generation_ + 1)) return std::unexpected(Error::AlreadyBound);
    owner_ = &target;
    ++generation_;
    return {};
}
auto Sc::bind(object::ref<Sc> self, Thread& target) noexcept -> Result {
    Result result;
    { sync::Lock guard{lock_};
      if (attaching_) return std::unexpected(Error::Active);
      result = bind_locked(std::move(self), target); }
    if (result) target.home_->request_control(target);
    return result;
}
auto Sc::bind_authorized(object::ref<Sc> self, Thread& target,
    const cap::Resolved<Sc>& context, const cap::Resolved<Thread>& thread) noexcept -> Result {
    std::optional<auth::flight> publishing;
    auth* authority;
    {
        sync::Lock guard{lock_};
        if (withdrawing_ || owner_ || attaching_) return std::unexpected(Error::AlreadyBound);
        if (auth_) {
            if (!auth_->reusable()) return std::unexpected(Error::Active);
            auth_.reset();
        }
        authority = &auth_.emplace(*this);
        publishing.emplace(*authority);
        attaching_ = true;
    }
    const auto attached = authority->attach(context, thread);
    Result result = std::unexpected(Error::Active);
    {
        sync::Lock guard{lock_};
        sync::Lock auth_guard{authority->lock_};
        if (attached && !authority->revoking_) result = bind_locked(std::move(self), target);
        attaching_ = false;
    }
    if (!result) authority->release();
    else target.home_->request_control(target);
    return result;
}
auto Sc::withdraw(u64 generation) noexcept -> Result {
    sync::Lock guard{lock_};
    if (!owner_) return std::unexpected(Error::NotBound);
    if (generation != generation_) return std::unexpected(Error::Stale);
    sync::Lock target_guard{owner_->lock_};
    owner_->detach_sc_ = true;
    owner_->home_->post(*owner_, Dispatcher::Control);
    return owner_->home_->kick_remote() ? Result{} : Result{std::unexpected(Error::Active)};
}
void Sc::release(Thread& target) noexcept {
    std::optional<auth::flight> publishing;
    auth* binding{};
    {
        sync::Lock guard{lock_};
        libk_assert(owner_ == &target);
        owner_ = nullptr;
        applied_ = 0;
        if (location_) lag_ = service_ > location_->fair_.load() ? service_ - location_->fair_.load() : 0;
        if (auth_) { binding = &*auth_; publishing.emplace(*binding); }
    }
    static_cast<void>(changed_.signal());
    if (binding) binding->release();
    post_close();
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
        if (attaching_) return;
        if (auth_) { sync::Lock auth_guard{auth_->lock_}; withdraw = auth_->revoking_; }
        withdraw = withdraw || withdrawing_;
        if (owner_) {
            if (withdraw) {
                sync::Lock target_guard{owner_->lock_};
                owner_->detach_sc_ = true;
                owner_->home_->post(*owner_, Dispatcher::Control);
                static_cast<void>(owner_->home_->kick_remote());
            }
            return;
        }
    }
    {
        sync::Lock guard{lock_};
        if (owner_) return;
        if (auth_) { if (!auth_->reusable()) return; auth_.reset(); }
        if (!withdrawing_) return;
    }
    { sync::Lock guard{lock_}; if (domain_) libk_assert(domain_->unadmit(*this)); }
    if (domain_cap_.attached()) static_cast<void>(domain_cap_.detach());
    domain_work_.reset();
    if (domain_cap_.busy()) return;
    object::cleanup done;
    { sync::Lock guard{lock_}; done = std::move(cleanup_); }
    if (done) { work_->close(close_work_); done.complete(); }
}

} // namespace sched
