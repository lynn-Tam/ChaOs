#pragma once

#include <libk/expected.hpp>
#include <libk/noncopyable.hpp>
#include <libk/sync/ticket_spin_lock.hpp>
#include <object/object_ref.hpp>
#include <resource/allocation.hpp>
#include <resource/sponsorship.hpp>

namespace kernel::cap {
class GrantGraph;
}

namespace kernel::resource {

enum class PoolState : u8 {
    Open,
    Closing,
    Revoking,
    Stopping,
    Reclaiming,
    Closed,
};

// ResourcePool owns promises, not pages or object payloads. Actual resources
// remain owned by PMM/ObjectStore/CSpace/GrantGraph; Sponsorship connects their
// true reuse point back to this ledger.
class ResourcePool final : private libk::noncopyable_nonmovable {
public:
    explicit ResourcePool(Budget limit) noexcept
        : limit_(limit), available_(limit) {}
    ~ResourcePool() noexcept;

    [[nodiscard]] auto reserve(
        kernel::object::ObjectRef self,
        Budget charge) noexcept -> libk::Expected<Reservation, PoolError>;
    [[nodiscard]] auto begin(
        kernel::object::ObjectRef self) noexcept
        -> libk::Expected<Permit, PoolError>;

    [[nodiscard]] auto limit() const noexcept -> Budget;
    [[nodiscard]] auto available() const noexcept -> Budget;
    [[nodiscard]] auto sponsorship_count() const noexcept -> usize;
    [[nodiscard]] auto state() const noexcept -> PoolState;
    [[nodiscard]] auto close() noexcept -> PoolState;
    [[nodiscard]] auto observe_refund(
        const kernel::object::ObjectRef& self,
        RefundNotifier notifier) noexcept -> bool;
    [[nodiscard]] auto can_retire() const noexcept -> bool;

private:
    friend class Reservation;
    friend class Permit;
    friend class Refund;
    friend class Charge;
    friend class Sponsorship;
    friend class Allocation;
    friend class kernel::cap::GrantGraph;

    [[nodiscard]] auto reserve(
        const Sponsorship& parent,
        Budget charge) noexcept -> libk::Expected<Reservation, PoolError>;
    [[nodiscard]] auto deduct(
        kernel::object::ObjectAnchor& anchor,
        u64 generation,
        Budget charge) noexcept -> libk::Expected<Reservation, PoolError>;
    void cancel(Reservation& reservation) noexcept;
    void commit(Reservation& reservation) noexcept;
    void finish(Permit& permit) noexcept;
    void commit(Allocation& allocation) noexcept;
    void ready(Allocation& allocation) noexcept;
    void target_ready(Allocation& allocation) noexcept;
    void child_closed(Allocation& allocation) noexcept;
    void bind_parent(Allocation& allocation) noexcept;
    void unbind_parent(Allocation& allocation) noexcept;
    void attach(Permit& permit, Allocation& allocation) noexcept;
    void detach(Allocation& allocation) noexcept;
    void attach(Sponsorship& sponsorship) noexcept;
    void detach(Sponsorship& sponsorship) noexcept;
    void refund(Refund& refund) noexcept;
    void refund(Charge& charge) noexcept;
    void service() noexcept;
    [[nodiscard]] auto closed_locked() const noexcept -> bool;

    mutable libk::TicketSpinLock lock_{};
    Budget limit_{};
    Budget available_{};
    PoolState state_{PoolState::Open};
    Allocation* roots_{};
    usize root_count_{};
    Sponsorship* sponsorships_{};
    usize sponsorship_count_{};
    usize reservation_count_{};
    usize construction_count_{};
    bool servicing_{};
    bool service_pending_{};
    Allocation* parent_{};
    bool parent_notified_{};
};

} // namespace kernel::resource
