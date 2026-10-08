#include <expected>
#include <object/group.hpp>

#include <ipc/channel.hpp>
#include <mm/pager.hpp>
#include <irq/irq.hpp>
#include <sched/sc.hpp>
#include <task/thread.hpp>

#include <cap/grant.hpp>
#include <panic.hpp>
#include <limits>
#include <utility>
#include <object/ref.hpp>
#include <sync.hpp>

namespace resource {
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
    auto ref = ref_.clone();
    libk_assert(ref);
    amount_.memory -= part.memory;
    amount_.caps -= part.caps;
    Charge result;
    result.owner_ = owner_;
    result.ref_ = std::move(ref).value();
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
    libk_assert(!previous_ && !next_ && !notifier_);
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
    auto ref = payer.clone();
    if (!owner || !ref) return std::unexpected(errc::invalid);
    auto reserved = owner->get().reserve(std::move(*ref), amount);
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
    libk_assert(!previous_ && !next_);
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

group::group(mm::Pmm& pmm, budget limit) noexcept
    : account_(limit),
      allocations_(pmm) {}

auto group::allocation_charge() noexcept -> budget {
    return budget{.memory = cap::Graph::node_charge().memory
        + decltype(allocations_)::slot_size()};
}

group::~group() noexcept {
    libk_assert(root_count_ == 0 && roots_ == nullptr);
    libk_assert(sponsorship_count_ == 0 && sponsorships_ == nullptr);
    libk_assert(construction_count_ == 0);
    libk_assert(state_ == phase::closed);
    libk_assert(!servicing_);
    libk_assert(parent_ == nullptr && !parent_notified_);
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

auto group::begin(ref<> self) noexcept -> std::expected<Txn, errc> {
    auto pin = self.as<group>();
    if (!pin || &pin->get() != this) return std::unexpected(errc::invalid);
    auto ref = self.clone();
    if (!ref) return std::unexpected(errc::invalid);
    const auto cost = allocation_charge();
    {
        sync::Lock guard{lock_};
        if (state_ != phase::open) return std::unexpected(errc::closed);
        if (!account_.reserve(cost)) return std::unexpected(errc::exhausted);
        libk_assert(construction_count_ != std::numeric_limits<usize>::max());
        ++construction_count_;
    }
    Reservation fee;
    fee.owner_ = this;
    fee.ref_ = std::move(self);
    fee.charge_ = cost;
    auto made = allocations_.create({});
    if (!made) {
        finish();
        return std::unexpected(errc::exhausted);
    }
    auto& item = **made;
    item.phase_ = allocation::phase::pending;
    attach(item);
    Txn txn;
    txn.h_ = decltype(txn.h_){Txn::Data{this, std::move(*ref), &item, std::move(fee), {}}};
    return txn;
}

auto group::reserve(
    const Sponsorship& parent,
    budget charge) noexcept -> std::expected<Reservation, errc> {
    if (parent.owner_ != this) return std::unexpected(errc::invalid);
    auto ref = parent.ref_.clone();
    if (!ref) return std::unexpected(errc::invalid);
    return reserve(std::move(ref).value(), charge);
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

auto group::can_retire() const noexcept -> bool {
    sync::Lock guard{lock_};
    return state_ == phase::closed && closed_locked();
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

void group::finish() noexcept {
    {
        sync::Lock guard{lock_};
        libk_assert(construction_count_ != 0);
        --construction_count_;
    }
    service();
}

void group::commit(allocation& allocation) noexcept {
    libk_assert(allocation.owner_ == this);
    sync::Lock guard{lock_};
    libk_assert(allocation.phase_ == allocation::phase::pending);
    libk_assert(construction_count_ != 0);
    allocation.phase_ = allocation::phase::live;
}

void group::ready(allocation& allocation) noexcept {
    {
        sync::Lock guard{lock_};
        libk_assert(allocation.owner_ == this);
        libk_assert(allocation.phase_ == allocation::phase::revoking);
        allocation.phase_ = allocation::phase::revoked;
    }
    service();
}

void group::target_ready(allocation& allocation) noexcept {
    {
        sync::Lock guard{lock_};
        libk_assert(allocation.owner_ == this);
        libk_assert(allocation.phase_ == allocation::phase::stopping);
        allocation.phase_ = allocation::phase::stopped;
    }
    service();
}

void group::close_allocation(allocation& allocation) noexcept {
    cap::Graph* graph{};
    {
        sync::Lock guard{lock_};
        libk_assert(allocation.owner_ == this);
        // The caller's accepted grant lease pins this allocation before its
        // revoke barrier. A concurrent pool close may already own the request.
        libk_assert(allocation.phase_ == allocation::phase::live || allocation.phase_ == allocation::phase::revoking);
        if (allocation.phase_ == allocation::phase::live) {
            allocation.independent_close_ = true;
            allocation.phase_ = allocation::phase::revoking;
            graph = allocation.graph_;
        }
    }
    if (graph != nullptr) graph->revoke_allocation(allocation);
}

void group::child_closed(allocation& allocation) noexcept {
    cap::Graph* graph{};
    {
        sync::Lock guard{lock_};
        libk_assert(allocation.owner_ == this);
        switch (allocation.phase_) {
        case allocation::phase::live:
            allocation.independent_close_ = true;
            allocation.phase_ = allocation::phase::revoking;
            graph = allocation.graph_;
            break;
        case allocation::phase::revoking:
            // The parent pool is already revoking this lineage. Its normal
            // stop phase observes that the child is already Closed.
            break;
        case allocation::phase::revoked:
            allocation.phase_ = allocation::phase::stopped;
            break;
        case allocation::phase::stopping:
            allocation.phase_ = allocation::phase::stopped;
            break;
        case allocation::phase::stopped:
        case allocation::phase::retiring:
            break;
        case allocation::phase::empty:
        case allocation::phase::pending:
            libk_assert(false);
            break;
        }
    }
    if (graph != nullptr) {
        graph->revoke_allocation(allocation);
    } else {
        service();
    }
}

void group::bind_parent(allocation& allocation) noexcept {
    sync::Lock guard{lock_};
    libk_assert(parent_ == nullptr && !parent_notified_);
    libk_assert(state_ == phase::open);
    parent_ = &allocation;
}

void group::unbind_parent(allocation& allocation) noexcept {
    sync::Lock guard{lock_};
    libk_assert(parent_ == &allocation);
    libk_assert(parent_notified_);
    libk_assert(state_ == phase::closed);
    parent_ = nullptr;
    parent_notified_ = false;
}

void group::attach(allocation& allocation) noexcept {
    sync::Lock guard{lock_};
    // A close may have changed Open to Closing after this creation entered.
    // construction_count_ is the barrier that prevents Revoking from passing
    // this transaction before its allocation root is registered.
    libk_assert(state_ == phase::open || state_ == phase::closing);
    libk_assert(construction_count_ != 0);
    libk_assert(allocation.owner_ == nullptr);
    libk_assert(allocation.previous_ == nullptr && allocation.next_ == nullptr);
    allocation.owner_ = this;
    allocation.next_ = roots_;
    if (roots_ != nullptr) {
        roots_->previous_ = &allocation;
    }
    roots_ = &allocation;
    ++root_count_;
}

void group::detach(allocation& allocation) noexcept {
    libk_assert(allocation.owner_ == this);
    {
        sync::Lock guard{lock_};
        if (allocation.previous_ != nullptr) {
            allocation.previous_->next_ = allocation.next_;
        } else {
            libk_assert(roots_ == &allocation);
            roots_ = allocation.next_;
        }
        if (allocation.next_ != nullptr) {
            allocation.next_->previous_ = allocation.previous_;
        }
        allocation.previous_ = nullptr;
        allocation.next_ = nullptr;
        allocation.owner_ = nullptr;
        libk_assert(root_count_ != 0);
        --root_count_;
    }
    service();
}

void group::attach(Sponsorship& sponsorship) noexcept {
    libk_assert(sponsorship.owner_ == this);
    sync::Lock guard{lock_};
    libk_assert(sponsorship.previous_ == nullptr && sponsorship.next_ == nullptr);
    sponsorship.next_ = sponsorships_;
    if (sponsorships_ != nullptr) {
        sponsorships_->previous_ = &sponsorship;
    }
    sponsorships_ = &sponsorship;
    ++sponsorship_count_;
    account_.commit();
}

void group::detach(Sponsorship& sponsorship) noexcept {
    libk_assert(sponsorship.owner_ == this);
    {
        sync::Lock guard{lock_};
        if (sponsorship.previous_ != nullptr) {
            sponsorship.previous_->next_ = sponsorship.next_;
        } else {
            libk_assert(sponsorships_ == &sponsorship);
            sponsorships_ = sponsorship.next_;
        }
        if (sponsorship.next_ != nullptr) {
            sponsorship.next_->previous_ = sponsorship.previous_;
        }
        sponsorship.previous_ = nullptr;
        sponsorship.next_ = nullptr;
        libk_assert(sponsorship_count_ != 0);
        --sponsorship_count_;
    }
    // Detach is called while the resource owner's container may still be
    // locked. The returned Refund retains both capacity and the pool; its
    // completion, after the resource is reusable, drives external close work.
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
    return root_count_ == 0 && roots_ == nullptr
        && sponsorship_count_ == 0 && sponsorships_ == nullptr
        && construction_count_ == 0 && account_.drained();
}

void group::service() noexcept {
    {
        sync::Lock guard{lock_};
        if (servicing_) {
            return;
        }
        servicing_ = true;
    }

    for (;;) {
        allocation* revoke{};
        allocation* stop{};
        allocation* retire{};
        allocation* notify_parent{};
        bool done{};
        {
            sync::Lock guard{lock_};
            // Claim under the same lock as completion transitions. Every
            // callback either precedes this scan or starts another service.
            const auto take = [&](allocation::phase ready, allocation::phase claimed,
                                  bool& waiting) noexcept -> allocation* {
                for (auto* item = roots_; item != nullptr; item = item->next_) {
                    libk_assert(item->phase_ >= ready);
                    if (item->phase_ == ready) {
                        item->phase_ = claimed;
                        return item;
                    }
                    waiting |= item->phase_ == claimed;
                }
                return nullptr;
            };
            bool advance = true;
            while (advance) {
                advance = false;

                // One pending local close must not block another allocation or
                // the pool-wide revoke barrier that may let it finish.
                for (allocation* allocation = roots_;
                     allocation != nullptr; allocation = allocation->next_) {
                    if (!allocation->independent_close_) continue;
                    switch (allocation->phase_) {
                    case allocation::phase::revoking:
                    case allocation::phase::stopping:
                    case allocation::phase::retiring:
                        continue;
                    case allocation::phase::revoked:
                        allocation->phase_ = allocation::phase::stopping;
                        stop = allocation;
                        break;
                    case allocation::phase::stopped:
                        allocation->phase_ = allocation::phase::retiring;
                        retire = allocation;
                        break;
                    case allocation::phase::empty:
                    case allocation::phase::pending:
                    case allocation::phase::live:
                        libk_assert(false);
                        break;
                    }
                    break;
                }
                if (stop != nullptr || retire != nullptr) break;

                switch (state_) {
                case phase::open:
                    break;
                case phase::closing:
                    if (account_.pending() == 0
                        && construction_count_ == 0) {
                        state_ = phase::revoking;
                        advance = true;
                    }
                    break;
                case phase::revoking: {
                    bool waiting{};
                    revoke = take(allocation::phase::live, allocation::phase::revoking, waiting);
                    if (revoke == nullptr && !waiting) {
                        state_ = phase::stopping;
                        advance = true;
                    }
                    break;
                }
                case phase::stopping: {
                    bool waiting{};
                    stop = take(allocation::phase::revoked, allocation::phase::stopping, waiting);
                    if (stop == nullptr && !waiting) {
                        state_ = phase::reclaiming;
                        advance = true;
                    }
                    break;
                }
                case phase::reclaiming: {
                    bool waiting{};
                    retire = take(allocation::phase::stopped, allocation::phase::retiring, waiting);
                    if (retire == nullptr && !waiting && closed_locked()) {
                        state_ = phase::closed;
                        advance = true;
                    }
                    break;
                }
                case phase::closed:
                    if (parent_ != nullptr && !parent_notified_) {
                        parent_notified_ = true;
                        notify_parent = parent_;
                    }
                    break;
                }
            }

            if (revoke == nullptr && stop == nullptr && retire == nullptr
                && notify_parent == nullptr) {
                servicing_ = false;
                done = true;
            }
        }

        if (done) {

            return;
        }

        if (revoke != nullptr) {
            libk_assert(revoke->graph_ != nullptr);
            revoke->graph_->revoke_allocation(*revoke);
        } else if (stop != nullptr) {
            stop->stop();
        } else if (retire != nullptr) {
            retire->retire();
        } else {
            libk_assert(notify_parent != nullptr);
            notify_parent->child_closed();
        }
    }
}

allocation::allocation() noexcept
    : revoke_(sync::Latch::Notifier::bind<
          &allocation::ready>(*this)),
      stop_(Stop::Notifier::bind<
          &allocation::target_ready>(*this)) {}

void allocation::ready() noexcept {
    libk_assert(owner_ != nullptr && revoke_.complete());
    owner_->ready(*this);
}

void allocation::target_ready() noexcept {
    libk_assert(owner_ != nullptr);
    owner_->target_ready(*this);
}

void allocation::child_closed() noexcept {
    libk_assert(owner_ != nullptr);
    owner_->child_closed(*this);
}

allocation::~allocation() noexcept {
    libk_assert(phase_ == phase::empty);
    libk_assert(owner_ == nullptr && graph_ == nullptr && !root_.valid());
    libk_assert(!target_ && previous_ == nullptr && next_ == nullptr);
    libk_assert(!stop_.started() || stop_.complete());
    libk_assert(!independent_close_);
}

void allocation::commit() noexcept {
    libk_assert(graph_ != nullptr && root_.valid());
    libk_assert(owner_ != nullptr);
    if (target_.kind() == object::ObjectKind::group) {
        auto child = target_.as<group>();
        libk_assert(child);
        child.value()->bind_parent(*this);
    }
    owner_->commit(*this);
}

void allocation::abort() noexcept {
    auto* pool = owner_;
    libk_assert(pool);
    if (!target_) {
        pool->detach(*this);
        phase_ = phase::empty;
        pool->allocations_.destroy(*this);
    } else if (graph_) {
        commit();
        pool->close_allocation(*this);
    } else {
        // No grant lineage was published: stop the private target directly.
        // The same allocation owns callbacks until its resources are reusable.
        {
            sync::Lock guard{pool->lock_};
            independent_close_ = true;
            phase_ = phase::revoked;
        }
        pool->service();
    }
}

void allocation::stop() noexcept {
    libk_assert(owner_ != nullptr && target_);
    libk_assert(phase_ == phase::stopping);

    switch (target_.kind()) {
    case object::ObjectKind::Thread: {
        auto thread = target_.as<Thread>();
        libk_assert(thread);
        libk_assert(!stop_.started());
        stop_.start(thread.value().get());
        return;
    }
    case object::ObjectKind::group: {
        auto child = target_.as<group>();
        libk_assert(child);
        if (child.value()->close() == group::phase::closed
            && phase_ == phase::stopping) {
            target_ready();
        }
        return;
    }
    case object::ObjectKind::Sc: {
        auto context = target_.as<sched::Sc>();
        libk_assert(context);
        if (!context.value()->bound()) {
            target_ready();
            return;
        }
        libk_assert(!stop_.started());
        stop_.start(context.value()->thread());
        return;
    }
    case object::ObjectKind::Invalid:
    case object::ObjectKind::Count:
        break;
    case object::ObjectKind::Domain:
    case object::ObjectKind::CSpace:
    case object::ObjectKind::IoSpace:
    case object::ObjectKind::Host:
    case object::ObjectKind::Mem:
    case object::ObjectKind::VSpace:
    case object::ObjectKind::Notification:
    case object::ObjectKind::Endpoint:
        target_ready();
        return;
    case object::ObjectKind::Channel: {
        auto channel = target_.as<ipc::Channel>();
        libk_assert(channel);
        static_cast<void>(channel.value()->close(cap::ChannelSide::A));
        static_cast<void>(channel.value()->close(cap::ChannelSide::B));
        target_ready();
        return;
    }
    case object::ObjectKind::Pager: {
        auto pager = target_.as<Pager>();
        libk_assert(pager);
        static_cast<void>(pager.value()->close(true));
        target_ready();
        return;
    }
    case object::ObjectKind::Irq: {
        auto irq = target_.as<irq::Irq>();
        libk_assert(irq);
        static_cast<void>(irq.value()->close());
        target_ready();
        return;
    }
    }
    libk_assert(false);
}

void allocation::retire() noexcept {
    libk_assert(owner_ != nullptr && target_);

    if (target_.kind() == object::ObjectKind::group) {
        auto child = target_.as<group>();
        libk_assert(child);
        if (child.value()->close() != group::phase::closed) {
            return;
        }
        if (graph_) child.value()->unbind_parent(*this);
    }
    if (stop_.started()) {
        libk_assert(stop_.started() && stop_.complete());
    }
    // Stopping has already drained every execution relation in this pool.
    // A false pre-retire result here means a target kind is missing an
    // explicit stop/dependency adapter; waiting silently would deadlock the
    // pool in Retiring with no event capable of re-driving it.
    const bool retired = target_.retire();
    libk_assert(retired);
    target_.reset();

    auto* const graph = graph_;
    auto* const pool = owner_;
    const auto root = root_;
    pool->detach(*this);
    graph_ = nullptr;
    root_ = {};
    independent_close_ = false;
    phase_ = phase::empty;
    // Keep the grant reclaim guard until its control slot is reusable.
    pool->allocations_.destroy(*this);
    if (graph) graph->release_allocation(root, this);
}

void group::Txn::Drop::operator()(Data& d) const noexcept {
    if (d.item) d.item->abort();
    d.root.reset();
    d.owner->finish();
}

auto group::Txn::adopt(cap::Graph& graph, ref<>&& target, cap::View ceiling) noexcept
    -> std::expected<void, cap::GrantError> {
    libk_assert(h_ && h_.get().item && !h_.get().root);
    const auto& d = h_.get();
    auto& item = *d.item;
    if (!target || item.target_)
        return std::unexpected(cap::GrantError::InvalidState);
    item.target_ = std::move(target);
    return root(graph, ceiling);
}

void group::Txn::own(ref<>&& target) noexcept {
    libk_assert(h_ && h_.get().item && !h_.get().item->target_ && target);
    h_.get().item->target_ = std::move(target);
}

auto group::Txn::root(cap::Graph& graph, cap::View ceiling) noexcept
    -> std::expected<void, cap::GrantError> {
    libk_assert(h_ && h_.get().item && !h_.get().root);
    auto& item = *h_.get().item;
    auto ref = item.target_.clone();
    if (!ref) return std::unexpected(cap::GrantError::InvalidState);
    auto owned = h_.release();
    auto created = graph.create_root(std::move(owned.fee), std::move(*ref), ceiling);
    if (created) {
        item.graph_ = &graph;
        item.root_ = created->key();
        graph.bind_allocation(item.root_, item);
        owned.root = std::move(*created);
    }
    h_ = decltype(h_){std::move(owned)};
    if (!created) return std::unexpected(created.error());
    return {};
}

auto group::Txn::acquire() const noexcept -> std::expected<cap::GrantLease, cap::GrantError> {
    if (!h_ || !h_.get().root) return std::unexpected(cap::GrantError::InvalidState);
    return h_.get().root.acquire();
}

auto group::Txn::derive(cap::GrantLease& root, cap::View view) noexcept
    -> std::expected<cap::GrantRef, Error> {
    const auto& d = h_.get();
    auto self = d.self.clone();
    if (!self) return std::unexpected(cap::GrantError::InvalidState);
    auto fee = d.owner->reserve(std::move(*self), cap::Graph::node_charge());
    if (!fee) return std::unexpected(fee.error());
    auto target = d.item->target_.clone();
    if (!target) return std::unexpected(cap::GrantError::InvalidState);
    auto grant = d.item->graph_->derive(std::move(*fee), root, std::move(*target), view);
    if (!grant) return std::unexpected(grant.error());
    return std::move(*grant);
}

void group::Txn::commit() noexcept {
    libk_assert(h_ && h_.get().item && h_.get().root);
    auto owned = h_.release();
    owned.item->commit();
    owned.item = nullptr;
    owned.root.reset();
    owned.owner->finish();
}

} // namespace object
