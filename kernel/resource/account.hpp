#pragma once

#include <libk/types.hpp>
#include <resource/budget.hpp>
#include <expected>
#include <array>
#include <libk/scope_guard.hpp>
#include <uapi/abi.h>
#include <libk/noncopyable.hpp>
#include <object/ref.hpp>
#include <wait.hpp>



namespace resource {

class Account;
class Funding;
class Charge;

// An empty payer is an uncharged internal allocation. A nonempty payer must
// name an open Account; the returned charge owns its own structural reference.
[[nodiscard]] auto acquire(const object::ref<>& payer, budget amount) noexcept
    -> std::expected<Charge, errc>;

// A pre-commit budget deduction. It also owns the structural reference that
// will keep the sponsor alive for the complete sponsored allocation lifetime.
class Reservation final : private libk::noncopyable {
public:
    Reservation() noexcept = default;
    Reservation(Reservation&& other) noexcept;
    auto operator=(Reservation&& other) noexcept -> Reservation&;
    ~Reservation() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept {
        return owner_ != nullptr;
    }

    [[nodiscard]] auto charge() const noexcept -> budget { return charge_; }
    [[nodiscard]] auto payer() const noexcept -> const object::ref<>& { return ref_; }
    // Commits this deduction without creating a new sponsored object. The
    // returned token must follow the concrete reusable resource (a page,
    // stack lease, queue cell, ...), and refunds only when that resource is
    // actually available again.
    [[nodiscard]] auto commit() && noexcept -> Charge;
    void reset() noexcept;

private:
    friend class Account;
    friend class Funding;

    Account* owner_{};
    object::ref<> ref_{};
    budget charge_{};
};

// Deferred refund token. Funding is detached before its object storage slot
// becomes reusable; this token releases the charge only after the actual slot
// and backing capacity have crossed that reuse boundary.
class Refund final : private libk::noncopyable {
public:
    Refund() noexcept = default;
    Refund(Refund&& other) noexcept;
    auto operator=(Refund&& other) noexcept -> Refund&;
    ~Refund() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept {
        return owner_ != nullptr;
    }

    void complete() noexcept;

private:
    friend class Funding;
    friend class Account;
    Account* owner_{};
    object::ref<> ref_{};
    budget charge_{};
};

// A movable claim for capacity already deducted from an Account.  Charge
// is paired with the real resource owner and may be split or merged as that
// ownership moves between containers.  Destroying it refunds capacity, so it
// must be destroyed only after the represented resource becomes reusable.
class Charge final : private libk::noncopyable {
public:
    Charge() noexcept = default;
    Charge(Charge&& other) noexcept;
    auto operator=(Charge&& other) noexcept -> Charge&;
    ~Charge() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept {
        return owner_ != nullptr;
    }
    [[nodiscard]] auto amount() const noexcept -> budget { return amount_; }
    auto payer() const noexcept -> const object::ref<>& { return ref_; }

    [[nodiscard]] auto split(budget part) noexcept -> Charge;
    void merge(Charge&& other) noexcept;
    // The paused owner has prepaid its complete actual fee set. Publication
    // swaps only prepared refs; Funding transfers the aggregate commitment.
    void reassign(const Charge& prepaid, object::ref<>& payer) noexcept {
        libk_assert(owner_ && prepaid.owner_ && prepaid.amount_.contains(amount_)
            && payer && payer.get() == prepaid.owner_);
        owner_ = prepaid.owner_; std::swap(ref_, payer);
    }
    void reset() noexcept;

private:
    friend class Reservation;
    friend class Funding;
    friend class Account;
    friend class object::Control;
    Account* owner_{};
    object::ref<> ref_{};
    budget amount_{};
};

// Storage fee only; lifecycle membership is independent.
class Funding final : private libk::noncopyable_nonmovable {
    friend class object::Scope;
    friend class object::Control;
public:
    Funding() noexcept = default;
    ~Funding() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept {
        return owner_ != nullptr;
    }

    [[nodiscard]] auto charge() const noexcept -> budget { return charge_; }
    [[nodiscard]] auto payer() const noexcept -> const object::ref<>& { return ref_; }
    // Mints a child charge from the same canonical pool. The returned
    // reservation owns a new structural hold; this attachment remains the
    // lineage fact but is not borrowed by the child allocation.
    [[nodiscard]] auto reserve(budget charge) const noexcept
        -> std::expected<Reservation, errc>;
    // Creates an independently movable subcharge.  Unlike Funding, a
    // Charge is not a new allocation root; it follows physical ownership and
    // keeps the sponsoring pool alive until its resource is really reusable.
    [[nodiscard]] auto acquire(budget charge) const noexcept
        -> std::expected<Charge, errc>;
    void commit(Reservation&& reservation) noexcept;
    void bind_control(object::Control&) noexcept;
    auto epoch() const noexcept -> u64 { sync::Lock guard{object::Control::lock}; return owner_ ? epoch_ : 0; }
    auto pause(const object::ref<>& payer, u64 epoch) noexcept -> status_t;
    void resume() noexcept;
    template<class Commit, class Publish> auto reassign(const object::ref<>& target, const object::ref<>& from,
        const object::ref<>& to, u64 epoch, Charge& replacement, Refund& refund, Commit&&, Publish&&) noexcept
        -> std::expected<u64, status_t>;
    [[nodiscard]] auto detach() noexcept -> Refund;

private:
    friend class Account;
    Account* owner_{};
    object::ref<> ref_{};
    budget charge_{};
    object::Control* control_{};
    u64 epoch_{1};
    bool rebilling_{};
};

// Capacity delegation is independent from Scope membership and Grant trees.
class Account final : private libk::noncopyable_nonmovable {
public:
    enum class Phase : u8 { Open = OBJECT_OPEN, Closing = OBJECT_CLOSING, Drained = OBJECT_DRAINED };
    explicit Account(budget limit, Charge delegation = {}) noexcept;
    auto control() noexcept -> object::Control& { return control_; }
    template<class Transaction> auto billing(const object::ref<>& to, Transaction&& transaction) noexcept
        -> std::expected<u64, status_t> {
        auto next = to.as<Account>();
        if (!next) return std::unexpected(STATUS_INVALID_CAP);
        {
            sync::Lock guard{lock_};
            if (phase_ == Phase::Drained || finishing_ || billing_) return std::unexpected(STATUS_BUSY);
            billing_ = true;
        }
        libk::scope_exit resume{[&]() noexcept {
            { sync::Lock guard{lock_}; billing_ = false; }
            service();
        }};
        return transaction([&](auto&& visit) { visit(delegation_); }, [] {});
    }
    ~Account() noexcept;
    auto reserve(object::ref<> self, budget amount) noexcept -> std::expected<Reservation, errc>;
    template<class Publish> auto resize(const object::ref<>& self, budget target,
        const object::ref<>& sponsor, budget ceiling, Publish&& publish) noexcept -> status_t;
    auto info(u64) const noexcept -> AccountInfo;
    auto limit() const noexcept -> budget;
    auto available() const noexcept -> budget;
    auto state() const noexcept -> Phase;
    auto close() noexcept -> Phase;
    void retire(object::cleanup&&) noexcept;
    auto events() noexcept -> Source& { return events_; }
    void bind_work(WorkQueue& work, object::Key key) noexcept { events_.bind_work(work, key); }
private:
    friend class Reservation;
    friend class Refund;
    friend class Charge;
    friend class Funding;
    friend class object::Control;
    object::Control control_;
    void cancel(budget) noexcept;
    void commit() noexcept;
    void attach(Funding&) noexcept;
    void detach(Funding&) noexcept;
    void refund(budget) noexcept;
    void service(bool close = false, object::cleanup cleanup = {}) noexcept;
    void complete_cleanup() noexcept;
    mutable sync::Spin lock_;
    budget limit_, available_;
    usize pending_{}, allocations_{};
    Phase phase_{Phase::Open};
    bool finishing_{}, billing_{};
    Charge delegation_;
    Src<STATE_OPEN | STATE_CLOSING | STATE_DRAINED> events_{Source::Drained::bind<&Account::complete_cleanup>(*this)};
    usize calls_{};
    object::cleanup cleanup_;
};

// Cold transfer of unused capacity. Funding and delegation are the sole
// lineage facts; reserve/refund retain their existing local cost.
template<class Publish>
auto Account::resize(const object::ref<>& self, budget target, const object::ref<>& sponsor,
    budget ceiling, Publish&& publish) noexcept -> status_t {
    if (self.get() != this) return STATUS_BAD_ARGS;
    auto* funding = self.funding();
    object::ref<> parent;
    {
        sync::Lock structure{object::Control::lock};
        if (!funding || !funding->owner_) return STATUS_DENIED;
        parent = funding->ref_;
    }
    // Prepare the possible zero-to-nonzero Charge before the commit locks.
    object::ref<> prepared = parent, released;
    auto& payer = *static_cast<Account*>(parent.get());
    Source::Publication publication;
    bool changed{};
    libk::scope_exit notify{[&]() noexcept {
        publication.publish();
        if (changed) { payer.service(); service(); object::changed(self.key()); }
    }};
    sync::Lock structure{object::Control::lock};
    if (funding->ref_.id() != parent.id()) return STATUS_STALE;
    if (funding->rebilling_) return STATUS_BUSY;
    auto* first = self.id().value < parent.id().value ? this : &payer;
    auto* second = first == this ? &payer : this;
    sync::Lock first_lock{first->lock_};
    sync::Lock second_lock{second->lock_};
    if (billing_ || finishing_) return STATUS_BUSY;
    if (phase_ == Phase::Drained) return STATUS_CLOSED;
    libk_assert((limit_.empty() && !delegation_) ||
        (delegation_.owner_ == &payer && delegation_.amount_ == limit_));
    const budget committed{limit_.memory - available_.memory, limit_.caps - available_.caps};
    if (!target.contains(committed)) return STATUS_BUSY;
    const budget up{target.memory > limit_.memory ? target.memory - limit_.memory : 0,
        target.caps > limit_.caps ? target.caps - limit_.caps : 0};
    const budget down{target.memory < limit_.memory ? limit_.memory - target.memory : 0,
        target.caps < limit_.caps ? limit_.caps - target.caps : 0};
    const bool grow = !up.empty();
    if (grow) {
        if (phase_ != Phase::Open || payer.phase_ != Phase::Open) return STATUS_CLOSED;
        if (sponsor.id() != parent.id() || !ceiling.contains(up)) return STATUS_DENIED;
        if (!payer.available_.contains(up)) return STATUS_NO_MEMORY;
    }
    std::array<const object::ref<>*, 2> gates{&self, &parent};
    const auto result = publish(std::span{gates.data(), grow ? 2u : 1u}, [&] {
        payer.available_.memory = payer.available_.memory - up.memory + down.memory;
        payer.available_.caps = payer.available_.caps - up.caps + down.caps;
        available_ = {target.memory - committed.memory, target.caps - committed.caps};
        limit_ = target;
        if (target.empty()) {
            released = std::move(delegation_.ref_);
            delegation_.owner_ = nullptr;
        } else if (!delegation_) {
            delegation_.owner_ = &payer;
            delegation_.ref_ = std::move(prepared);
        }
        delegation_.amount_ = target;
        publication = events_.prepare(static_cast<u32>(phase_) == OBJECT_OPEN ? STATE_OPEN : STATE_CLOSING, true);
        changed = true;
    });
    return result;
}

template<class Commit, class Publish>
auto Funding::reassign(const object::ref<>& target, const object::ref<>& from, const object::ref<>& to,
    u64 epoch, Charge& replacement, Refund& refund, Commit&& commit, Publish&& publish) noexcept -> std::expected<u64, status_t> {
    auto old_account = from.as<Account>(), new_account = to.as<Account>();
    if (!old_account || !new_account || from.id() == to.id()) return std::unexpected(STATUS_BAD_ARGS);
    auto& old = old_account->get();
    auto& next = new_account->get();
    bool changed{};
    libk::scope_exit notify{[&]() noexcept { if (changed) object::changed(target.key()); }};
    sync::Lock structure{object::Control::lock};
    if (!rebilling_ || owner_ != &old || epoch_ != epoch) return std::unexpected(STATUS_STALE);
    if (epoch_ == std::numeric_limits<u64>::max()) return std::unexpected(STATUS_NO_MEMORY);
    if (control_) {
        auto cycle = next.control_.reaches(*control_);
        if (!cycle) return std::unexpected(STATUS_NO_MEMORY);
        if (*cycle) return std::unexpected(STATUS_DENIED);
    }
    auto* first = from.id().value < to.id().value ? &old : &next;
    auto* second = first == &old ? &next : &old;
    sync::Lock first_lock{first->lock_};
    sync::Lock second_lock{second->lock_};
    if (next.phase_ != Account::Phase::Open) return std::unexpected(STATUS_CLOSED);
    libk_assert(replacement.owner_ == &next && replacement.amount_.contains(charge_) && !refund);
    std::array<const object::ref<>*, 3> gates{&target, &from, &to};
    const auto status = publish(std::span{gates}, [&] {
        // Every replacement reference was prepared before taking Gate locks.
        // Commit only moves actual fee tokens and indices; refunds happen later.
        libk_assert(old.allocations_ && next.allocations_ != std::numeric_limits<usize>::max());
        --old.allocations_; ++next.allocations_;
        refund.owner_ = owner_; refund.ref_ = std::move(ref_); refund.charge_ = replacement.amount_;
        owner_ = replacement.owner_;
        ref_ = std::move(replacement.ref_);
        commit();
        replacement.owner_ = nullptr; replacement.amount_ = {};
        ++epoch_;
    });
    if (status != STATUS_OK) return std::unexpected(status);
    changed = true;
    return epoch_;
}

} // namespace resource
