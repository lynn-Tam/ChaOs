#pragma once

#include <core/types.hpp>
#include <ipc/notification.hpp>
#include <libk/expected.hpp>
#include <libk/noncopyable.hpp>
#include <object/object_cleanup.hpp>
#include <sync/lock.hpp>

namespace kernel::irq {

// Called once the boot CPU has installed a valid trap entry.  Before that
// point construction tests may exercise Irq state without touching MMIO.
void initialize_platform() noexcept;

// A token is minted by the platform/bootstrap layer.  User code can carry it
// only through a typed Irq construction; the integer is never a capability.
class SourceToken final {
public:
    SourceToken() noexcept = default;

    [[nodiscard]] static constexpr auto from_bootstrap(
        u32 source,
        bool level = true) noexcept -> SourceToken {
        return SourceToken{source, level};
    }
    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return valid_;
    }
    [[nodiscard]] constexpr auto id() const noexcept -> u32 { return id_; }
    [[nodiscard]] constexpr auto level() const noexcept -> bool { return level_; }

private:
    constexpr SourceToken(u32 id, bool level) noexcept
        : id_(id), level_(level), valid_(id != 0) {}

    u32 id_{};
    bool level_{};
    bool valid_{};
};

enum class State : u8 {
    UnboundIdle,
    UnboundPending,
    BoundIdle,
    BoundPending,
    Closing,
    Closed,
};

enum class Error : u8 {
    InvalidSource,
    InvalidState,
    Busy,
    StaleSequence,
    BadSequence,
    Closed,
};

struct Delivery final {
    u64 sequence{};
    u64 generation{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return sequence != 0 && generation != 0;
    }
};

// Irq owns the hardware observation state.  Notification is only the
// retained readiness projection and never carries pending/ack truth.
class Irq final : private libk::noncopyable_nonmovable {
public:
    explicit Irq(SourceToken source) noexcept;
    ~Irq() noexcept;

    [[nodiscard]] auto source() const noexcept -> SourceToken { return source_; }
    [[nodiscard]] auto state() const noexcept -> State;
    [[nodiscard]] auto delivery_sequence() const noexcept -> u64;
    [[nodiscard]] auto bind(ipc::Notification& notification, u64 badge) noexcept
        -> libk::Expected<void, Error>;
    [[nodiscard]] auto unbind() noexcept -> bool;
    // Returns the delivery published by the trap path without advancing it.
    // User space consumes the notification projection first, then reads this
    // immutable sequence and acknowledges that exact publication.
    [[nodiscard]] auto delivery() const noexcept
        -> libk::Expected<Delivery, Error>;
    [[nodiscard]] auto observe() noexcept -> libk::Expected<Delivery, Error>;
    [[nodiscard]] auto ack(u64 generation, u64 sequence) noexcept
        -> libk::Expected<void, Error>;
    // Platform trap path: dispatches one normalized hardware source into the
    // single Irq object registered for it.  The registry is fixed-capacity.
    static void dispatch(u32 source) noexcept;
    [[nodiscard]] auto close() noexcept -> bool;
    void retire(object::ObjectCleanup&& cleanup) noexcept;

private:
    friend void initialize_platform() noexcept;

    /* The registry lock is held by callers of both helpers.  Keeping the
     * observe/close pair in this lock-coupled form lets the trap path retire
     * a sequence-exhausted source without reopening the raw-pointer lifetime
     * gap. */
    [[nodiscard]] auto observe_locked() noexcept
        -> libk::Expected<Delivery, Error>;
    [[nodiscard]] auto close_locked(
        ipc::Notification*& notification) noexcept -> bool;
    void finish_close(ipc::Notification* notification) noexcept;
    void notification_closed() noexcept;

    SourceToken source_{};
    mutable kernel::sync::SpinLock<kernel::sync::LockClass::Irq>
        lock_{};
    ipc::NotificationSource source_link_;
    ipc::Notification* notification_{};
    State state_{State::UnboundIdle};
    u64 generation_{};
    u64 sequence_{};
    bool observed_since_ack_{};
    object::ObjectCleanup cleanup_{};
};

} // namespace kernel::irq
