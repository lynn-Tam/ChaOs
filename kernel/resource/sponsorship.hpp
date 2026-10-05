#pragma once

#include <base/types.hpp>
#include <resource/budget.hpp>
#include <expected>
#include <libk/noncopyable.hpp>
#include <object/ref.hpp>

namespace object { class group; }

namespace resource {

class Sponsorship;
class Charge;

// A refund happens after the sponsored object has been destroyed. Delivery
// therefore owns a value, never a callback into that object's payload.
class RefundNotifier final {
public:
    template<auto Method, typename Owner>
    static auto bind(Owner& owner) noexcept -> RefundNotifier {
        return {&owner, [](void* context, usize, u64, u64) noexcept {
            (static_cast<Owner*>(context)->*Method)();
        }, 0, 0, 0};
    }
    using Deliver = void (*)(void*, usize, u64, u64) noexcept;
    RefundNotifier() noexcept = default;
    RefundNotifier(void* context, Deliver deliver, usize slot, u64 generation, u64 badge) noexcept
        : context_(context), deliver_(deliver), slot_(slot), generation_(generation), badge_(badge) {}
    explicit operator bool() const noexcept { return deliver_ != nullptr; }
    void operator()() const noexcept { deliver_(context_, slot_, generation_, badge_); }
private:
    void* context_{};
    Deliver deliver_{};
    usize slot_{};
    u64 generation_{}, badge_{};
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
        return owner_ != nullptr;
    }

    [[nodiscard]] auto charge() const noexcept -> budget { return charge_; }
    // Commits this deduction without creating a new sponsored object. The
    // returned token must follow the concrete reusable resource (a page,
    // stack lease, queue cell, ...), and refunds only when that resource is
    // actually available again.
    [[nodiscard]] auto commit() && noexcept -> Charge;
    void reset() noexcept;

private:
    friend class object::group;
    friend class Sponsorship;

    object::group* owner_{};
    object::ref<> ref_{};
    budget charge_{};
};

// Deferred refund token. Sponsorship is detached before its object storage slot
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
    friend class Sponsorship;
    friend class object::group;
    object::group* owner_{};
    object::ref<> ref_{};
    budget charge_{};
    RefundNotifier notifier_{};
};

// A movable claim for capacity already deducted from an object group.  Charge
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

    [[nodiscard]] auto split(budget part) noexcept -> Charge;
    void merge(Charge&& other) noexcept;
    void reset() noexcept;

private:
    friend class Reservation;
    friend class Sponsorship;
    friend class object::group;
    object::group* owner_{};
    object::ref<> ref_{};
    budget amount_{};
};

// Owned by the object storage slot: one sponsored object has exactly one primary
// sponsor and one charge. List links are only a non-owning pool index.
class Sponsorship final : private libk::noncopyable_nonmovable {
public:
    Sponsorship() noexcept = default;
    ~Sponsorship() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept {
        return owner_ != nullptr;
    }

    [[nodiscard]] auto charge() const noexcept -> budget { return charge_; }
    // Mints a child charge from the same canonical pool. The returned
    // reservation owns a new structural hold; this attachment remains the
    // lineage fact but is not borrowed by the child allocation.
    [[nodiscard]] auto reserve(budget charge) const noexcept
        -> std::expected<Reservation, errc>;
    // Creates an independently movable subcharge.  Unlike Sponsorship, a
    // Charge is not a new allocation root; it follows physical ownership and
    // keeps the sponsoring pool alive until its resource is really reusable.
    [[nodiscard]] auto acquire(budget charge) const noexcept
        -> std::expected<Charge, errc>;
    void commit(Reservation&& reservation) noexcept;
    [[nodiscard]] auto detach() noexcept -> Refund;
    [[nodiscard]] auto observe_refund(RefundNotifier notifier) noexcept
        -> bool;

private:
    friend class object::group;
    object::group* owner_{};
    object::ref<> ref_{};
    budget charge_{};
    RefundNotifier notifier_{};
    Sponsorship* previous_{};
    Sponsorship* next_{};
};

} // namespace resource
