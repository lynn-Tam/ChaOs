#pragma once

#include <core/types.hpp>
#include <libk/delegate.hpp>
#include <libk/noncopyable.hpp>

namespace kernel::object {
class ObjectAnchor;
class ObjectRef;
}

namespace kernel::cap {
class GrantGraph;
}

namespace kernel::resource {

class ResourcePool;
class Sponsorship;
class Permit;
class Charge;

using RefundNotifier = libk::delegate<void() noexcept>;

struct Budget final {
    u64 memory{};
    u64 caps{};

    [[nodiscard]] constexpr auto empty() const noexcept -> bool {
        return memory == 0 && caps == 0;
    }

    friend constexpr auto operator==(Budget, Budget) noexcept
        -> bool = default;
};

enum class PoolError : u8 {
    InvalidPool,
    Closed,
    Exhausted,
    Overflow,
    OutstandingAllocations,
};

// Pins the Open construction epoch across a complete publish transaction.
// Budget reservations protect capacity; Permit protects the control-root gap
// between object publication and allocation registration.
class Permit final : private libk::noncopyable {
public:
    Permit() noexcept = default;
    Permit(Permit&& other) noexcept;
    auto operator=(Permit&& other) noexcept -> Permit&;
    ~Permit() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept {
        return pool_ != nullptr;
    }
    void reset() noexcept;

private:
    friend class ResourcePool;
    friend class kernel::cap::GrantGraph;

    ResourcePool* pool_{};
    kernel::object::ObjectAnchor* anchor_{};
    u64 generation_{};
};

// A pre-commit budget deduction. It also owns the structural reference that
// will keep the sponsor alive for the complete sponsored allocation lifetime.
class Reservation final : private libk::noncopyable {
public:
    Reservation() noexcept = default;
    Reservation(Reservation&& other) noexcept;
    auto operator=(Reservation&& other) noexcept -> Reservation&;
    ~Reservation() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept {
        return pool_ != nullptr;
    }

    [[nodiscard]] auto charge() const noexcept -> Budget { return charge_; }
    // Commits this deduction without creating a new sponsored object. The
    // returned token must follow the concrete reusable resource (a page,
    // stack lease, queue cell, ...), and refunds only when that resource is
    // actually available again.
    [[nodiscard]] auto commit() && noexcept -> Charge;
    void reset() noexcept;

private:
    friend class ResourcePool;
    friend class Sponsorship;

    ResourcePool* pool_{};
    kernel::object::ObjectAnchor* anchor_{};
    u64 generation_{};
    Budget charge_{};
};

// Deferred refund token. Sponsorship is detached before its ObjectAnchor slot
// becomes reusable; this token releases the charge only after the actual slot
// and backing capacity have crossed that reuse boundary.
class Refund final : private libk::noncopyable {
public:
    Refund() noexcept = default;
    Refund(Refund&& other) noexcept;
    auto operator=(Refund&& other) noexcept -> Refund&;
    ~Refund() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept {
        return pool_ != nullptr;
    }

    void complete() noexcept;

private:
    friend class Sponsorship;
    friend class ResourcePool;

    ResourcePool* pool_{};
    kernel::object::ObjectAnchor* anchor_{};
    u64 generation_{};
    Budget charge_{};
    RefundNotifier notifier_{};
};

// A movable claim for capacity already deducted from a ResourcePool.  Charge
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
        return pool_ != nullptr;
    }
    [[nodiscard]] auto budget() const noexcept -> Budget { return budget_; }

    [[nodiscard]] auto split(Budget part) noexcept -> Charge;
    void merge(Charge&& other) noexcept;
    void reset() noexcept;

private:
    friend class Reservation;
    friend class Sponsorship;
    friend class ResourcePool;

    ResourcePool* pool_{};
    kernel::object::ObjectAnchor* anchor_{};
    u64 generation_{};
    Budget budget_{};
};

// Embedded in ObjectAnchor: one sponsored object has exactly one primary
// sponsor and one charge. List links are only a non-owning pool index.
class Sponsorship final : private libk::noncopyable_nonmovable {
public:
    Sponsorship() noexcept = default;
    ~Sponsorship() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept {
        return pool_ != nullptr;
    }

    [[nodiscard]] auto charge() const noexcept -> Budget { return charge_; }
    // Mints a child charge from the same canonical pool. The returned
    // reservation owns a new structural hold; this attachment remains the
    // lineage fact but is not borrowed by the child allocation.
    [[nodiscard]] auto reserve(Budget charge) const noexcept
        -> libk::Expected<Reservation, PoolError>;
    // Creates an independently movable subcharge.  Unlike Sponsorship, a
    // Charge is not a new allocation root; it follows physical ownership and
    // keeps the sponsoring pool alive until its resource is really reusable.
    [[nodiscard]] auto acquire(Budget charge) const noexcept
        -> libk::Expected<Charge, PoolError>;
    void commit(Reservation&& reservation) noexcept;
    [[nodiscard]] auto detach() noexcept -> Refund;
    [[nodiscard]] auto observe_refund(RefundNotifier notifier) noexcept
        -> bool;

private:
    friend class ResourcePool;

    ResourcePool* pool_{};
    kernel::object::ObjectAnchor* anchor_{};
    u64 generation_{};
    Budget charge_{};
    RefundNotifier notifier_{};
    Sponsorship* previous_{};
    Sponsorship* next_{};
};

} // namespace kernel::resource
