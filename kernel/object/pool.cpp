#include <expected>
#include <object/pool.hpp>


#include <cap/grant.hpp>
#include <panic.hpp>
#include <limits>
#include <utility>
#include <object/ref.hpp>
#include <sync.hpp>

namespace resource {
account::~account() noexcept { libk_assert(drained()); }

auto account::reserve(budget amount) noexcept -> bool {
    if (!available_.contains(amount)) return false;
    libk_assert(pending_ != std::numeric_limits<usize>::max());
    available_.memory -= amount.memory;
    available_.caps -= amount.caps;
    ++pending_;
    return true;
}

void account::commit() noexcept {
    libk_assert(pending_ != 0);
    --pending_;
}

void account::cancel(budget amount) noexcept {
    refund(amount);
    commit();
}

void account::refund(budget amount) noexcept {
    libk_assert(limit_.memory - available_.memory >= amount.memory);
    libk_assert(limit_.caps - available_.caps >= amount.caps);
    available_.memory += amount.memory;
    available_.caps += amount.caps;
}


Reservation::Reservation(Reservation&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), ref_(std::move(other.ref_)),
      charge_(std::exchange(other.charge_, budget{})) {}

auto Reservation::operator=(Reservation&& other) noexcept -> Reservation& {
    if (this != &other) {
        reset();
        owner_ = std::exchange(other.owner_, nullptr);
        ref_ = std::move(other.ref_);
        charge_ = std::exchange(other.charge_, budget{});
    }
    return *this;
}

Reservation::~Reservation() noexcept { reset(); }

Refund::Refund(Refund&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), ref_(std::move(other.ref_)),
      charge_(std::exchange(other.charge_, budget{})),
      notifier_(std::exchange(other.notifier_, RefundNotifier{})) {}

auto Refund::operator=(Refund&& other) noexcept -> Refund& {
    if (this != &other) {
        complete();
        owner_ = std::exchange(other.owner_, nullptr);
        ref_ = std::move(other.ref_);
        charge_ = std::exchange(other.charge_, budget{});
        notifier_ = std::exchange(other.notifier_, RefundNotifier{});
    }
    return *this;
}

Refund::~Refund() noexcept { complete(); }

Charge::Charge(Charge&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), ref_(std::move(other.ref_)),
      amount_(std::exchange(other.amount_, budget{})) {}

auto Charge::operator=(Charge&& other) noexcept -> Charge& {
    if (this != &other) {
        reset();
        owner_ = std::exchange(other.owner_, nullptr);
        ref_ = std::move(other.ref_);
        amount_ = std::exchange(other.amount_, budget{});
    }
    return *this;
}

Charge::~Charge() noexcept { reset(); }

void Reservation::reset() noexcept {
    if (!owner_) { libk_assert(!ref_ && charge_.empty()); return; }
    libk_assert(ref_);
    owner_->cancel(charge_);
    owner_ = nullptr;
    charge_ = {};
    ref_.reset();
}

auto Reservation::commit() && noexcept -> Charge {
    libk_assert(owner_ && ref_ && !charge_.empty());
    owner_->commit();
    Charge result;
    result.owner_ = std::exchange(owner_, nullptr);
    result.ref_ = std::move(ref_);
    result.amount_ = std::exchange(charge_, budget{});
    return result;
}

void Refund::complete() noexcept {
    if (!owner_) { libk_assert(!ref_ && charge_.empty() && !notifier_); return; }
    libk_assert(ref_);
    owner_->refund(charge_);
    owner_ = nullptr;
    charge_ = {};
    ref_.reset();
    const auto notify = std::exchange(notifier_, RefundNotifier{});
    if (notify) notify();
}

auto Charge::split(budget part) noexcept -> Charge {
    libk_assert(owner_ && ref_ && amount_.contains(part) && !part.empty());
    if (part == amount_) return Charge{std::move(*this)};
    amount_.memory -= part.memory;
    amount_.caps -= part.caps;
    Charge result;
    result.owner_ = owner_;
    result.ref_ = ref_;
    result.amount_ = part;
    return result;
}

void Charge::merge(Charge&& other) noexcept {
    if (!other) return;
    if (!*this) { *this = std::move(other); return; }
    libk_assert(owner_ == other.owner_ && ref_.id() == other.ref_.id());
    libk_assert(std::numeric_limits<u64>::max() - amount_.memory >= other.amount_.memory);
    libk_assert(std::numeric_limits<u64>::max() - amount_.caps >= other.amount_.caps);
    amount_.memory += other.amount_.memory;
    amount_.caps += other.amount_.caps;
    other.owner_ = nullptr;
    other.amount_ = {};
    other.ref_.reset();
}

void Charge::reset() noexcept {
    if (!owner_) { libk_assert(!ref_ && amount_.empty()); return; }
    libk_assert(ref_ && !amount_.empty());
    owner_->refund(amount_);
    owner_ = nullptr;
    amount_ = {};
    ref_.reset();
}

Sponsorship::~Sponsorship() noexcept {
    libk_assert(!owner_ && !ref_ && charge_.empty());
    libk_assert(!notifier_);
}

void Sponsorship::commit(Reservation&& reservation) noexcept {
    libk_assert(!owner_ && !ref_ && reservation);
    owner_ = std::exchange(reservation.owner_, nullptr);
    ref_ = std::move(reservation.ref_);
    charge_ = std::exchange(reservation.charge_, budget{});
    owner_->attach(*this);
}

auto Sponsorship::reserve(budget charge) const noexcept
    -> std::expected<Reservation, errc> {
    if (!owner_) return std::unexpected(errc::invalid);
    return owner_->reserve(*this, charge);
}

auto acquire(const object::ref<>& payer, budget amount) noexcept
    -> std::expected<Charge, errc> {
    if (!payer) return Charge{};
    auto owner = payer.as<object::group>();
    if (!owner) return std::unexpected(errc::invalid);
    auto reserved = owner->get().reserve(payer, amount);
    if (!reserved) return std::unexpected(reserved.error());
    return std::move(*reserved).commit();
}

auto Sponsorship::acquire(budget amount) const noexcept
    -> std::expected<Charge, errc> {
    if (!owner_) return std::unexpected(errc::invalid);
    return resource::acquire(ref_, amount);
}

auto Sponsorship::detach() noexcept -> Refund {
    if (!owner_) return {};
    owner_->detach(*this);
    Refund refund;
    refund.owner_ = std::exchange(owner_, nullptr);
    refund.ref_ = std::move(ref_);
    refund.charge_ = std::exchange(charge_, budget{});
    refund.notifier_ = std::exchange(notifier_, RefundNotifier{});
    return refund;
}

auto Sponsorship::observe_refund(RefundNotifier notifier) noexcept -> bool {
    if (!owner_ || !notifier || notifier_) return false;
    notifier_ = notifier;
    return true;
}

} // namespace resource

namespace object {
using resource::budget;
using resource::errc;
using resource::Reservation;
using resource::Sponsorship;
using resource::RefundNotifier;

group::group(budget limit) noexcept : account_(limit) {}


group::~group() noexcept {
    libk_assert(members_ == nullptr);
    libk_assert(sponsorship_count_ == 0);
    libk_assert(state_ == phase::closed);
    libk_assert(!servicing_);
    libk_assert(!cleanup_);
}

auto group::reserve(
    ref<> self,
    budget charge) noexcept -> std::expected<Reservation, errc> {
    auto pin = self.as<group>();
    if (!pin || &pin.value().get() != this) {
        return std::unexpected(errc::invalid);
    }

    {
        sync::Lock guard{lock_};
        if (state_ != phase::open) {
            return std::unexpected(errc::closed);
        }
        if (!account_.reserve(charge)) {
            return std::unexpected(errc::exhausted);
        }
    }

    Reservation reservation{};
    reservation.owner_ = this;
    reservation.ref_ = std::move(self);
    reservation.charge_ = charge;
    return reservation;
}

auto group::reserve(
    const Sponsorship& parent,
    budget charge) noexcept -> std::expected<Reservation, errc> {
    if (parent.owner_ != this) return std::unexpected(errc::invalid);
    return reserve(parent.ref_, charge);
}

auto group::limit() const noexcept -> budget {
    sync::Lock guard{lock_};
    return account_.limit();
}

auto group::available() const noexcept -> budget {
    sync::Lock guard{lock_};
    return account_.available();
}

auto group::sponsorship_count() const noexcept -> usize {
    sync::Lock guard{lock_};
    return sponsorship_count_;
}

auto group::state() const noexcept -> phase {
    sync::Lock guard{lock_};
    return state_;
}

auto group::close() noexcept -> phase {
    {
        sync::Lock guard{lock_};
        if (state_ == phase::open) {
            state_ = phase::closing;
        }
    }

    service();
    return state();
}

auto group::observe_refund(
    const ref<>& self,
    RefundNotifier notifier) noexcept -> bool {
    auto pin = self.as<group>();
    if (!pin || &pin.value().get() != this) {
        return false;
    }
    // The operation pin prevents sponsorship detach; this lock arbitrates
    // concurrent blocking/asynchronous observers of the same one-shot close.
    sync::Lock guard{lock_};
    return sponsor_ != nullptr && sponsor_->observe_refund(notifier);
}

void group::retire(cleanup&& done) noexcept {
    {
        sync::Lock guard{lock_};
        libk_assert(!cleanup_);
        cleanup_ = std::move(done);
        if (state_ == phase::open) state_ = phase::closing;
    }
    service();
}

void group::cancel(budget amount) noexcept {
    {
        sync::Lock guard{lock_};
        account_.cancel(amount);
    }
    service();
}

void group::commit() noexcept {
    {
        sync::Lock guard{lock_};
        account_.commit();
    }
    service();
}

void group::commit(Member& member) noexcept {
    {
        sync::Lock guard{lock_};
        libk_assert(member.owner_ == this && member.phase_ == Member::phase::pending);
        member.phase_ = Member::phase::live;
    }
    service();
}

void group::ready(Member& member) noexcept {
    {
        sync::Lock guard{lock_};
        libk_assert(member.owner_ == this);
        libk_assert(member.phase_ == Member::phase::revoking);
        member.phase_ = Member::phase::revoked;
    }
    service();
}

void group::close_member(Member& member) noexcept {
    cap::Graph* graph{};
    {
        sync::Lock guard{lock_};
        libk_assert(member.owner_ == this);
        // The caller's accepted grant lease pins this member before its
        // revoke barrier. A concurrent pool close may already own the request.
        // A repeated close joins the existing revoke/retirement obligation.
        if (member.phase_ != Member::phase::pending && member.phase_ != Member::phase::live)
            return;
        // A capability may be consumed immediately after batch publication.
        // Commit owns the transition out of construction and starts this close.
        if (member.phase_ == Member::phase::pending) member.independent_close_ = true;
        if (member.phase_ == Member::phase::live) {
            member.hold_ = member.pin();
            if (!member.hold_) return; // Last-ref retirement will detach it.
            member.independent_close_ = true;
            member.phase_ = Member::phase::revoking;
            graph = member.graph_;
        }
    }
    if (graph != nullptr) graph->close(member.root_, member.revoke_);
}

void group::attach(Member& member) noexcept {
    sync::Lock guard{lock_};
    // The accepted root Reservation covers this publication window. Pending
    // members stay in the list until publication or rollback, including close.
    libk_assert(state_ == phase::open || state_ == phase::closing);
    libk_assert(member.owner_ == nullptr);
    libk_assert(member.previous_ == nullptr && member.next_ == nullptr);
    member.owner_ = this;
    member.next_ = members_;
    if (members_ != nullptr) {
        members_->previous_ = &member;
    }
    members_ = &member;
}

void group::detach(Member& member) noexcept {
    libk_assert(member.owner_ == this);
    {
        sync::Lock guard{lock_};
        if (member.previous_ != nullptr) {
            member.previous_->next_ = member.next_;
        } else {
            libk_assert(members_ == &member);
            members_ = member.next_;
        }
        if (member.next_ != nullptr) {
            member.next_->previous_ = member.previous_;
        }
        member.previous_ = nullptr;
        member.next_ = nullptr;
        member.owner_ = nullptr;
    }
    service();
}

void group::attach(Sponsorship& sponsorship) noexcept {
    libk_assert(sponsorship.owner_ == this);
    sync::Lock guard{lock_};
    ++sponsorship_count_;
    account_.commit();
}

void group::detach(Sponsorship& sponsorship) noexcept {
    libk_assert(sponsorship.owner_ == this);
    sync::Lock guard{lock_};
    libk_assert(sponsorship_count_ != 0);
    --sponsorship_count_;
    // Refund, after the real storage reuse boundary, drives close work.
}

void group::refund(budget amount) noexcept {
    {
        sync::Lock guard{lock_};
        account_.refund(amount);
    }
    service();
}

auto group::closed_locked() const noexcept -> bool {
    libk_assert(lock_.held());
    return members_ == nullptr
        && sponsorship_count_ == 0
        && account_.drained();
}

void group::service() noexcept {
    {
        sync::Lock guard{lock_};
        if (servicing_) return;
        servicing_ = true;
    }
    for (;;) {
        Member* item{};
        cleanup done{};
        Member::phase step{};
        {
            sync::Lock guard{lock_};
            const bool closing = state_ == phase::closing
                && account_.pending() == 0;
            for (auto* entry = members_; entry; entry = entry->next_) {
                if ((closing || entry->independent_close_)
                    && (entry->phase_ == Member::phase::live
                        || entry->phase_ == Member::phase::revoked)) {
                    if (entry->phase_ == Member::phase::live) {
                        entry->hold_ = entry->pin();
                        if (!entry->hold_) continue;
                    }
                    item = entry;
                    break;
                }
            }
            if (closing && closed_locked()) state_ = phase::closed;
            if (item) {
                step = item->phase_;
                switch (step) {
                case Member::phase::live: item->phase_ = Member::phase::revoking; break;
                case Member::phase::revoked: item->phase_ = Member::phase::retiring; break;
                default: libk_assert(false);
                }
            } else {
                servicing_ = false;
                if (state_ == phase::closed) done = std::move(cleanup_);
            }
        }
        // Claim and completion share lock_. Callbacks may drive this loop but
        // never execute an owner or graph operation while that lock is held.
        if (!item) {
            if (done) done.complete();
            return;
        }
        if (step == Member::phase::live) item->graph_->close(item->root_, item->revoke_);
        else item->retire();
    }
}

Member::Member() noexcept
    : revoke_(sync::Latch::Notifier::bind<
          &Member::ready>(*this)) {}

void Member::ready() noexcept {
    libk_assert(owner_ != nullptr && revoke_.complete());
    owner_->ready(*this);
}

Member::~Member() noexcept {
    libk_assert(phase_ == phase::empty);
    libk_assert(owner_ == nullptr && graph_ == nullptr && !root_.valid());
    libk_assert(!target_.valid() && !hold_ && previous_ == nullptr && next_ == nullptr);
    libk_assert(!independent_close_);
}

void Member::commit() noexcept {
    libk_assert(graph_ != nullptr && root_.valid());
    libk_assert(owner_ != nullptr);
    owner_->commit(*this);
}

void Member::abort() noexcept {
    auto* pool = owner_;
    libk_assert(pool);
    libk_assert(target_.valid());
    if (graph_) {
        {
            sync::Lock guard{pool->lock_};
            independent_close_ = true;
        }
        commit();
    } else {
        // No grant lineage was published: retire the private target directly.
        // The same member owns callbacks until its resources are reusable.
        {
            sync::Lock guard{pool->lock_};
            independent_close_ = true;
            hold_ = pin();
            libk_assert(hold_);
            phase_ = phase::revoked;
        }
        pool->service();
    }
}

auto Member::pin() const noexcept -> ref<> {
    // The Group lock keeps this embedded member attached while the pool lock
    // arbitrates admission against last-ref retirement. No pool→Group lock edge.
    auto* anchor = reinterpret_cast<object::anchor*>(target_.slot);
    if (!anchor->ops_->try_retain(anchor->owner_, *anchor, target_.generation)) return {};
    return ref<>{*anchor, target_.generation};
}

void Member::retire() noexcept {
    libk_assert(owner_ && hold_ && phase_ == phase::retiring);
    auto target = std::move(hold_);
    libk_assert(target.retire());
}

void Member::detach() noexcept {
    libk_assert(!hold_);
    if (owner_) owner_->detach(*this);
    graph_ = nullptr;
    root_ = {};
    target_ = {};
    independent_close_ = false;
    phase_ = phase::empty;
}

auto Member::root(cap::Graph& graph, Reservation&& fee, ref<> target, cap::View view) noexcept
    -> std::expected<cap::GrantRef, cap::GrantError> {
    if (!fee || owner_ || phase_ != phase::empty)
        return std::unexpected(cap::GrantError::InvalidState);
    auto payer = fee.payer().as<group>();
    if (!payer) return std::unexpected(cap::GrantError::InvalidState);
    phase_ = phase::pending;
    target_ = target.id();
    payer->get().attach(*this);
    auto root = graph.create_root(std::move(fee), std::move(target), view);
    if (!root) return std::unexpected(root.error());
    graph_ = &graph;
    root_ = root->key();
    return root;
}

} // namespace object
