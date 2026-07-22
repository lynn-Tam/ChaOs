#include <resource/pool.hpp>

#include <cap/grant_graph.hpp>
#include <core/debug.hpp>
#include <libk/limits.hpp>
#include <libk/utility.hpp>
#include <object/resource_pool.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel::resource {
namespace {

[[nodiscard]] constexpr auto contains(Budget available, Budget charge) noexcept
    -> bool {
    return available.memory >= charge.memory && available.caps >= charge.caps;
}

} // namespace

Permit::Permit(Permit&& other) noexcept
    : pool_(libk::exchange(other.pool_, nullptr)),
      anchor_(libk::exchange(other.anchor_, nullptr)),
      generation_(libk::exchange(other.generation_, u64{})) {}

auto Permit::operator=(Permit&& other) noexcept -> Permit& {
    if (this == &other) {
        return *this;
    }
    reset();
    pool_ = libk::exchange(other.pool_, nullptr);
    anchor_ = libk::exchange(other.anchor_, nullptr);
    generation_ = libk::exchange(other.generation_, u64{});
    return *this;
}

Permit::~Permit() noexcept { reset(); }

void Permit::reset() noexcept {
    ResourcePool* const pool = libk::exchange(pool_, nullptr);
    kernel::object::ObjectAnchor* const anchor =
        libk::exchange(anchor_, nullptr);
    const u64 generation = libk::exchange(generation_, u64{});
    if (pool == nullptr) {
        KASSERT(anchor == nullptr && generation == 0);
        return;
    }
    KASSERT(anchor != nullptr && generation != 0);
    pool_ = pool;
    pool->finish(*this);
    pool_ = nullptr;
    anchor->ops_->drop_ref(anchor->owner_, *anchor, generation);
}

Reservation::Reservation(Reservation&& other) noexcept
    : pool_(libk::exchange(other.pool_, nullptr)),
      anchor_(libk::exchange(other.anchor_, nullptr)),
      generation_(libk::exchange(other.generation_, u64{})),
      charge_(libk::exchange(other.charge_, Budget{})) {}

auto Reservation::operator=(Reservation&& other) noexcept -> Reservation& {
    if (this == &other) {
        return *this;
    }
    reset();
    pool_ = libk::exchange(other.pool_, nullptr);
    anchor_ = libk::exchange(other.anchor_, nullptr);
    generation_ = libk::exchange(other.generation_, u64{});
    charge_ = libk::exchange(other.charge_, Budget{});
    return *this;
}

Reservation::~Reservation() noexcept { reset(); }

auto Reservation::commit() && noexcept -> Charge {
    KASSERT(pool_ != nullptr && anchor_ != nullptr && generation_ != 0);
    KASSERT(!charge_.empty());
    pool_->commit(*this);

    Charge result{};
    result.pool_ = libk::exchange(pool_, nullptr);
    result.anchor_ = libk::exchange(anchor_, nullptr);
    result.generation_ = libk::exchange(generation_, u64{});
    result.budget_ = libk::exchange(charge_, Budget{});
    return result;
}

void Reservation::reset() noexcept {
    ResourcePool* const pool = libk::exchange(pool_, nullptr);
    kernel::object::ObjectAnchor* const anchor =
        libk::exchange(anchor_, nullptr);
    const u64 generation = libk::exchange(generation_, u64{});
    if (pool == nullptr) {
        KASSERT(anchor == nullptr && generation == 0 && charge_.empty());
        return;
    }
    KASSERT(anchor != nullptr && generation != 0);
    pool_ = pool;
    pool->cancel(*this);
    pool_ = nullptr;
    charge_ = {};
    anchor->ops_->drop_ref(anchor->owner_, *anchor, generation);
}

Refund::Refund(Refund&& other) noexcept
    : pool_(libk::exchange(other.pool_, nullptr)),
      anchor_(libk::exchange(other.anchor_, nullptr)),
      generation_(libk::exchange(other.generation_, u64{})),
      charge_(libk::exchange(other.charge_, Budget{})),
      notifier_(libk::exchange(other.notifier_, RefundNotifier{})) {}

auto Refund::operator=(Refund&& other) noexcept -> Refund& {
    if (this == &other) {
        return *this;
    }
    complete();
    pool_ = libk::exchange(other.pool_, nullptr);
    anchor_ = libk::exchange(other.anchor_, nullptr);
    generation_ = libk::exchange(other.generation_, u64{});
    charge_ = libk::exchange(other.charge_, Budget{});
    notifier_ = libk::exchange(other.notifier_, RefundNotifier{});
    return *this;
}

Refund::~Refund() noexcept { complete(); }

void Refund::complete() noexcept {
    ResourcePool* const pool = libk::exchange(pool_, nullptr);
    kernel::object::ObjectAnchor* const anchor =
        libk::exchange(anchor_, nullptr);
    const u64 generation = libk::exchange(generation_, u64{});
    if (pool == nullptr) {
        KASSERT(anchor == nullptr && generation == 0 && charge_.empty());
        KASSERT(!notifier_);
        return;
    }
    KASSERT(anchor != nullptr && generation != 0);
    pool_ = pool;
    pool->refund(*this);
    pool_ = nullptr;
    charge_ = {};
    anchor->ops_->drop_ref(anchor->owner_, *anchor, generation);
    const RefundNotifier notifier = libk::exchange(
        notifier_, RefundNotifier{});
    if (notifier) {
        notifier();
    }
}

Charge::Charge(Charge&& other) noexcept
    : pool_(libk::exchange(other.pool_, nullptr)),
      anchor_(libk::exchange(other.anchor_, nullptr)),
      generation_(libk::exchange(other.generation_, u64{})),
      budget_(libk::exchange(other.budget_, Budget{})) {}

auto Charge::operator=(Charge&& other) noexcept -> Charge& {
    if (this == &other) {
        return *this;
    }
    reset();
    pool_ = libk::exchange(other.pool_, nullptr);
    anchor_ = libk::exchange(other.anchor_, nullptr);
    generation_ = libk::exchange(other.generation_, u64{});
    budget_ = libk::exchange(other.budget_, Budget{});
    return *this;
}

Charge::~Charge() noexcept { reset(); }

auto Charge::split(Budget part) noexcept -> Charge {
    KASSERT(pool_ != nullptr && anchor_ != nullptr && generation_ != 0);
    KASSERT(contains(budget_, part));
    KASSERT(!part.empty());
    if (part == budget_) {
        return Charge{libk::move(*this)};
    }
    KASSERT(anchor_->ops_->try_ref(anchor_->owner_, *anchor_, generation_));

    budget_.memory -= part.memory;
    budget_.caps -= part.caps;
    Charge result{};
    result.pool_ = pool_;
    result.anchor_ = anchor_;
    result.generation_ = generation_;
    result.budget_ = part;
    return result;
}

void Charge::merge(Charge&& other) noexcept {
    if (!other) {
        return;
    }
    if (!*this) {
        *this = libk::move(other);
        return;
    }
    KASSERT(pool_ == other.pool_ && anchor_ == other.anchor_);
    KASSERT(generation_ == other.generation_);
    KASSERT(libk::numeric_limits<u64>::max() - budget_.memory
        >= other.budget_.memory);
    KASSERT(libk::numeric_limits<u64>::max() - budget_.caps
        >= other.budget_.caps);
    budget_.memory += other.budget_.memory;
    budget_.caps += other.budget_.caps;

    kernel::object::ObjectAnchor* const redundant_anchor =
        libk::exchange(other.anchor_, nullptr);
    const u64 redundant_generation =
        libk::exchange(other.generation_, u64{});
    other.pool_ = nullptr;
    other.budget_ = {};
    redundant_anchor->ops_->drop_ref(
        redundant_anchor->owner_, *redundant_anchor, redundant_generation);
}

void Charge::reset() noexcept {
    ResourcePool* const pool = libk::exchange(pool_, nullptr);
    kernel::object::ObjectAnchor* const anchor =
        libk::exchange(anchor_, nullptr);
    const u64 generation = libk::exchange(generation_, u64{});
    if (pool == nullptr) {
        KASSERT(anchor == nullptr && generation == 0 && budget_.empty());
        return;
    }
    KASSERT(anchor != nullptr && generation != 0 && !budget_.empty());
    pool_ = pool;
    anchor_ = anchor;
    generation_ = generation;
    pool->refund(*this);
    pool_ = nullptr;
    anchor_ = nullptr;
    generation_ = 0;
    budget_ = {};
    anchor->ops_->drop_ref(anchor->owner_, *anchor, generation);
}

Sponsorship::~Sponsorship() noexcept {
    KASSERT(pool_ == nullptr);
    KASSERT(anchor_ == nullptr && generation_ == 0 && charge_.empty());
    KASSERT(previous_ == nullptr && next_ == nullptr);
    KASSERT(!notifier_);
}

void Sponsorship::commit(Reservation&& reservation) noexcept {
    KASSERT(pool_ == nullptr && reservation);
    pool_ = libk::exchange(reservation.pool_, nullptr);
    anchor_ = libk::exchange(reservation.anchor_, nullptr);
    generation_ = libk::exchange(reservation.generation_, u64{});
    charge_ = libk::exchange(reservation.charge_, Budget{});
    pool_->attach(*this);
}

auto Sponsorship::reserve(Budget charge) const noexcept
    -> libk::Expected<Reservation, PoolError> {
    if (pool_ == nullptr) {
        return libk::unexpected(PoolError::InvalidPool);
    }
    return pool_->reserve(*this, charge);
}

auto Sponsorship::acquire(Budget charge) const noexcept
    -> libk::Expected<Charge, PoolError> {
    auto reserved = reserve(charge);
    if (!reserved) {
        return libk::unexpected(reserved.error());
    }
    Reservation reservation = libk::move(reserved).value();
    reservation.pool_->commit(reservation);
    Charge result{};
    result.pool_ = libk::exchange(reservation.pool_, nullptr);
    result.anchor_ = libk::exchange(reservation.anchor_, nullptr);
    result.generation_ = libk::exchange(reservation.generation_, u64{});
    result.budget_ = libk::exchange(reservation.charge_, Budget{});
    return libk::expected(libk::move(result));
}

auto Sponsorship::detach() noexcept -> Refund {
    if (pool_ == nullptr) {
        return {};
    }
    pool_->detach(*this);
    Refund refund{};
    refund.pool_ = libk::exchange(pool_, nullptr);
    refund.anchor_ = libk::exchange(anchor_, nullptr);
    refund.generation_ = libk::exchange(generation_, u64{});
    refund.charge_ = libk::exchange(charge_, Budget{});
    refund.notifier_ = libk::exchange(notifier_, RefundNotifier{});
    KASSERT(previous_ == nullptr && next_ == nullptr);
    return refund;
}

auto Sponsorship::observe_refund(RefundNotifier notifier) noexcept -> bool {
    if (pool_ == nullptr || !notifier || notifier_) {
        return false;
    }
    notifier_ = notifier;
    return true;
}

ResourcePool::~ResourcePool() noexcept {
    KASSERT(root_count_ == 0 && roots_ == nullptr);
    KASSERT(sponsorship_count_ == 0 && sponsorships_ == nullptr);
    KASSERT(reservation_count_ == 0 && construction_count_ == 0);
    KASSERT(available_ == limit_);
    KASSERT(state_ == PoolState::Closed);
    KASSERT(!servicing_ && !service_pending_);
    KASSERT(parent_ == nullptr && !parent_notified_);
}

auto ResourcePool::reserve(
    kernel::object::ObjectRef self,
    Budget charge) noexcept -> libk::Expected<Reservation, PoolError> {
    auto pin = self.pin<ResourcePool>();
    if (!pin || &pin.value().get() != this) {
        return libk::unexpected(PoolError::InvalidPool);
    }

    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != PoolState::Open) {
            return libk::unexpected(PoolError::Closed);
        }
        if (!contains(available_, charge)) {
            return libk::unexpected(PoolError::Exhausted);
        }
        KASSERT(reservation_count_ != libk::numeric_limits<usize>::max());
        available_.memory -= charge.memory;
        available_.caps -= charge.caps;
        ++reservation_count_;
    }

    Reservation reservation{};
    reservation.pool_ = this;
    reservation.anchor_ = libk::exchange(self.anchor_, nullptr);
    reservation.generation_ = libk::exchange(self.generation_, u64{});
    reservation.charge_ = charge;
    return libk::expected(libk::move(reservation));
}

auto ResourcePool::begin(
    kernel::object::ObjectRef self) noexcept
    -> libk::Expected<Permit, PoolError> {
    auto pin = self.pin<ResourcePool>();
    if (!pin || &pin.value().get() != this) {
        return libk::unexpected(PoolError::InvalidPool);
    }
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != PoolState::Open) {
            return libk::unexpected(PoolError::Closed);
        }
        KASSERT(construction_count_ != libk::numeric_limits<usize>::max());
        ++construction_count_;
    }
    Permit permit{};
    permit.pool_ = this;
    permit.anchor_ = libk::exchange(self.anchor_, nullptr);
    permit.generation_ = libk::exchange(self.generation_, u64{});
    return libk::expected(libk::move(permit));
}

auto ResourcePool::reserve(
    const Sponsorship& parent,
    Budget charge) noexcept -> libk::Expected<Reservation, PoolError> {
    if (parent.pool_ != this || parent.anchor_ == nullptr
        || parent.generation_ == 0) {
        return libk::unexpected(PoolError::InvalidPool);
    }
    return deduct(*parent.anchor_, parent.generation_, charge);
}

auto ResourcePool::deduct(
    kernel::object::ObjectAnchor& anchor,
    u64 generation,
    Budget charge) noexcept -> libk::Expected<Reservation, PoolError> {
    if (!anchor.ops_->try_ref(anchor.owner_, anchor, generation)) {
        return libk::unexpected(PoolError::InvalidPool);
    }
    PoolError error{};
    bool failed{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != PoolState::Open || !contains(available_, charge)) {
            error = state_ != PoolState::Open
                ? PoolError::Closed
                : PoolError::Exhausted;
            failed = true;
        } else {
            KASSERT(reservation_count_ != libk::numeric_limits<usize>::max());
            available_.memory -= charge.memory;
            available_.caps -= charge.caps;
            ++reservation_count_;
        }
    }
    if (failed) {
        anchor.ops_->drop_ref(anchor.owner_, anchor, generation);
        return libk::unexpected(error);
    }

    Reservation reservation{};
    reservation.pool_ = this;
    reservation.anchor_ = &anchor;
    reservation.generation_ = generation;
    reservation.charge_ = charge;
    return libk::expected(libk::move(reservation));
}

auto ResourcePool::limit() const noexcept -> Budget {
    kernel::sync::IrqLockGuard guard{lock_};
    return limit_;
}

auto ResourcePool::available() const noexcept -> Budget {
    kernel::sync::IrqLockGuard guard{lock_};
    return available_;
}

auto ResourcePool::sponsorship_count() const noexcept -> usize {
    kernel::sync::IrqLockGuard guard{lock_};
    return sponsorship_count_;
}

auto ResourcePool::state() const noexcept -> PoolState {
    kernel::sync::IrqLockGuard guard{lock_};
    return state_;
}

auto ResourcePool::close() noexcept -> PoolState {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ == PoolState::Open) {
            state_ = PoolState::Closing;
        }
    }
    service();
    return state();
}

auto ResourcePool::observe_refund(
    const kernel::object::ObjectRef& self,
    RefundNotifier notifier) noexcept -> bool {
    auto pin = self.pin<ResourcePool>();
    if (!pin || &pin.value().get() != this || self.anchor_ == nullptr) {
        return false;
    }
    return self.anchor_->sponsorship_.observe_refund(notifier);
}

auto ResourcePool::can_retire() const noexcept -> bool {
    kernel::sync::IrqLockGuard guard{lock_};
    return state_ == PoolState::Closed
        && root_count_ == 0 && sponsorship_count_ == 0
        && reservation_count_ == 0
        && construction_count_ == 0 && available_ == limit_;
}

void ResourcePool::cancel(Reservation& reservation) noexcept {
    KASSERT(reservation.pool_ == this);
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(limit_.memory - available_.memory
            >= reservation.charge_.memory);
        KASSERT(limit_.caps - available_.caps >= reservation.charge_.caps);
        available_.memory += reservation.charge_.memory;
        available_.caps += reservation.charge_.caps;
        KASSERT(reservation_count_ != 0);
        --reservation_count_;
    }
    service();
}

void ResourcePool::commit(Reservation& reservation) noexcept {
    KASSERT(reservation.pool_ == this);
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(reservation_count_ != 0);
        --reservation_count_;
    }
    service();
}

void ResourcePool::finish(Permit& permit) noexcept {
    KASSERT(permit.pool_ == this);
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(construction_count_ != 0);
        --construction_count_;
    }
    service();
}

void ResourcePool::commit(Allocation& allocation) noexcept {
    KASSERT(allocation.pool_ == this);
    kernel::sync::IrqLockGuard guard{lock_};
    KASSERT(allocation.state_ == AllocationState::Pending);
    KASSERT(construction_count_ != 0);
    allocation.state_ = AllocationState::Live;
}

void ResourcePool::ready(Allocation& allocation) noexcept {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(allocation.pool_ == this);
        KASSERT(allocation.state_ == AllocationState::Revoking);
        allocation.state_ = AllocationState::Revoked;
    }
    service();
}

void ResourcePool::target_ready(Allocation& allocation) noexcept {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(allocation.pool_ == this);
        KASSERT(allocation.state_ == AllocationState::Stopping);
        allocation.state_ = AllocationState::Stopped;
    }
    service();
}

void ResourcePool::child_closed(Allocation& allocation) noexcept {
    kernel::cap::GrantGraph* graph{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(allocation.pool_ == this);
        switch (allocation.state_) {
        case AllocationState::Live:
            allocation.independent_close_ = true;
            allocation.state_ = AllocationState::Revoking;
            graph = allocation.graph_;
            break;
        case AllocationState::Revoking:
            // The parent pool is already revoking this lineage. Its normal
            // stop phase observes that the child is already Closed.
            break;
        case AllocationState::Revoked:
            allocation.state_ = AllocationState::Stopped;
            break;
        case AllocationState::Stopping:
            allocation.state_ = AllocationState::Stopped;
            break;
        case AllocationState::Stopped:
        case AllocationState::Retiring:
            break;
        case AllocationState::Empty:
        case AllocationState::Pending:
            KASSERT(false);
            break;
        }
    }
    if (graph != nullptr) {
        graph->revoke_allocation(allocation);
    } else {
        service();
    }
}

void ResourcePool::bind_parent(Allocation& allocation) noexcept {
    kernel::sync::IrqLockGuard guard{lock_};
    KASSERT(parent_ == nullptr && !parent_notified_);
    KASSERT(state_ == PoolState::Open);
    parent_ = &allocation;
}

void ResourcePool::unbind_parent(Allocation& allocation) noexcept {
    kernel::sync::IrqLockGuard guard{lock_};
    KASSERT(parent_ == &allocation);
    KASSERT(parent_notified_);
    KASSERT(state_ == PoolState::Closed);
    parent_ = nullptr;
    parent_notified_ = false;
}

void ResourcePool::attach(
    Permit& permit,
    Allocation& allocation) noexcept {
    KASSERT(permit.pool_ == this);
    kernel::sync::IrqLockGuard guard{lock_};
    // A close may have changed Open to Closing after this permit entered.
    // construction_count_ is the barrier that prevents Revoking from passing
    // this transaction before its allocation root is registered.
    KASSERT(state_ == PoolState::Open || state_ == PoolState::Closing);
    KASSERT(construction_count_ != 0);
    KASSERT(allocation.pool_ == nullptr);
    KASSERT(allocation.previous_ == nullptr && allocation.next_ == nullptr);
    allocation.pool_ = this;
    allocation.next_ = roots_;
    if (roots_ != nullptr) {
        roots_->previous_ = &allocation;
    }
    roots_ = &allocation;
    ++root_count_;
}

void ResourcePool::detach(Allocation& allocation) noexcept {
    KASSERT(allocation.pool_ == this);
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (allocation.previous_ != nullptr) {
            allocation.previous_->next_ = allocation.next_;
        } else {
            KASSERT(roots_ == &allocation);
            roots_ = allocation.next_;
        }
        if (allocation.next_ != nullptr) {
            allocation.next_->previous_ = allocation.previous_;
        }
        allocation.previous_ = nullptr;
        allocation.next_ = nullptr;
        allocation.pool_ = nullptr;
        KASSERT(root_count_ != 0);
        --root_count_;
    }
    service();
}

void ResourcePool::attach(Sponsorship& sponsorship) noexcept {
    KASSERT(sponsorship.pool_ == this);
    kernel::sync::IrqLockGuard guard{lock_};
    KASSERT(sponsorship.previous_ == nullptr && sponsorship.next_ == nullptr);
    sponsorship.next_ = sponsorships_;
    if (sponsorships_ != nullptr) {
        sponsorships_->previous_ = &sponsorship;
    }
    sponsorships_ = &sponsorship;
    ++sponsorship_count_;
    KASSERT(reservation_count_ != 0);
    --reservation_count_;
}

void ResourcePool::detach(Sponsorship& sponsorship) noexcept {
    KASSERT(sponsorship.pool_ == this);
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (sponsorship.previous_ != nullptr) {
            sponsorship.previous_->next_ = sponsorship.next_;
        } else {
            KASSERT(sponsorships_ == &sponsorship);
            sponsorships_ = sponsorship.next_;
        }
        if (sponsorship.next_ != nullptr) {
            sponsorship.next_->previous_ = sponsorship.previous_;
        }
        sponsorship.previous_ = nullptr;
        sponsorship.next_ = nullptr;
        KASSERT(sponsorship_count_ != 0);
        --sponsorship_count_;
    }
    service();
}

void ResourcePool::refund(Refund& refund) noexcept {
    KASSERT(refund.pool_ == this);
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(limit_.memory - available_.memory >= refund.charge_.memory);
        KASSERT(limit_.caps - available_.caps >= refund.charge_.caps);
        available_.memory += refund.charge_.memory;
        available_.caps += refund.charge_.caps;
    }
    service();
}

void ResourcePool::refund(Charge& charge) noexcept {
    KASSERT(charge.pool_ == this);
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(limit_.memory - available_.memory >= charge.budget_.memory);
        KASSERT(limit_.caps - available_.caps >= charge.budget_.caps);
        available_.memory += charge.budget_.memory;
        available_.caps += charge.budget_.caps;
    }
    service();
}

auto ResourcePool::closed_locked() const noexcept -> bool {
    kernel::sync::LockAccess::assert_held(lock_);
    return root_count_ == 0 && roots_ == nullptr
        && sponsorship_count_ == 0 && sponsorships_ == nullptr
        && reservation_count_ == 0 && construction_count_ == 0
        && available_ == limit_;
}

void ResourcePool::service() noexcept {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (servicing_) {
            service_pending_ = true;
            return;
        }
        servicing_ = true;
    }

    for (;;) {
        Allocation* revoke{};
        Allocation* stop{};
        Allocation* retire{};
        Allocation* notify_parent{};
        {
            kernel::sync::IrqLockGuard guard{lock_};
            service_pending_ = false;
            bool advance = true;
            while (advance) {
                advance = false;

                Allocation* independent{};
                for (Allocation* allocation = roots_;
                     allocation != nullptr;
                     allocation = allocation->next_) {
                    if (allocation->independent_close_) {
                        independent = allocation;
                        break;
                    }
                }
                if (independent != nullptr) {
                    switch (independent->state_) {
                    case AllocationState::Revoking:
                    case AllocationState::Retiring:
                        break;
                    case AllocationState::Revoked:
                        independent->state_ = AllocationState::Stopped;
                        advance = true;
                        break;
                    case AllocationState::Stopped:
                        independent->state_ = AllocationState::Retiring;
                        retire = independent;
                        break;
                    case AllocationState::Empty:
                    case AllocationState::Pending:
                    case AllocationState::Live:
                    case AllocationState::Stopping:
                        KASSERT(false);
                        break;
                    }
                    if (retire != nullptr || !advance) {
                        break;
                    }
                    continue;
                }

                switch (state_) {
                case PoolState::Open:
                    break;
                case PoolState::Closing:
                    if (reservation_count_ == 0
                        && construction_count_ == 0) {
                        state_ = PoolState::Revoking;
                        advance = true;
                    }
                    break;
                case PoolState::Revoking: {
                    bool waiting{};
                    for (Allocation* allocation = roots_;
                         allocation != nullptr;
                         allocation = allocation->next_) {
                        KASSERT(allocation->state_ != AllocationState::Pending);
                        if (allocation->state_ == AllocationState::Live) {
                            allocation->state_ = AllocationState::Revoking;
                            revoke = allocation;
                            break;
                        }
                        waiting = waiting
                            || allocation->state_ == AllocationState::Revoking;
                    }
                    if (revoke == nullptr && !waiting) {
                        state_ = PoolState::Stopping;
                        advance = true;
                    }
                    break;
                }
                case PoolState::Stopping: {
                    bool waiting{};
                    for (Allocation* allocation = roots_;
                         allocation != nullptr;
                         allocation = allocation->next_) {
                        KASSERT(allocation->state_ == AllocationState::Revoked
                            || allocation->state_ == AllocationState::Stopping
                            || allocation->state_ == AllocationState::Stopped);
                        if (allocation->state_ == AllocationState::Revoked) {
                            allocation->state_ = AllocationState::Stopping;
                            stop = allocation;
                            break;
                        }
                        waiting = waiting
                            || allocation->state_ == AllocationState::Stopping;
                    }
                    if (stop == nullptr && !waiting) {
                        state_ = PoolState::Reclaiming;
                        advance = true;
                    }
                    break;
                }
                case PoolState::Reclaiming: {
                    bool waiting{};
                    for (Allocation* allocation = roots_;
                         allocation != nullptr;
                         allocation = allocation->next_) {
                        KASSERT(allocation->state_ == AllocationState::Stopped
                            || allocation->state_ == AllocationState::Retiring);
                        if (allocation->state_ == AllocationState::Stopped) {
                            allocation->state_ = AllocationState::Retiring;
                            retire = allocation;
                            break;
                        }
                        waiting = true;
                    }
                    if (retire == nullptr && !waiting && closed_locked()) {
                        state_ = PoolState::Closed;
                        advance = true;
                    }
                    break;
                }
                case PoolState::Closed:
                    if (parent_ != nullptr && !parent_notified_) {
                        parent_notified_ = true;
                        notify_parent = parent_;
                    }
                    break;
                }
            }

            if (revoke == nullptr && stop == nullptr && retire == nullptr
                && notify_parent == nullptr) {
                if (service_pending_) {
                    continue;
                }
                servicing_ = false;
                return;
            }
        }

        if (revoke != nullptr) {
            KASSERT(revoke->graph_ != nullptr);
            revoke->graph_->revoke_allocation(*revoke);
        } else if (stop != nullptr) {
            KASSERT(stop->graph_ != nullptr);
            stop->graph_->stop_allocation(*stop);
        } else if (retire != nullptr) {
            KASSERT(retire != nullptr && retire->graph_ != nullptr);
            retire->graph_->retire_allocation(*retire);
        } else {
            KASSERT(notify_parent != nullptr);
            notify_parent->child_closed();
        }
    }
}

} // namespace kernel::resource
