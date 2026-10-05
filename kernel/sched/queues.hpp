#pragma once

#include <optional>


#include <libk/assert.hpp>
#include <base/types.hpp>
#include <libk/intrusive_list.hpp>
#include <sched/sc.hpp>
#include <sched/types.hpp>
#include <libk/delegate.hpp>
#include <libk/intrusive_tree.hpp>
#include <libk/noncopyable.hpp>
#include <time/time.hpp>

namespace sched {

// Home-CPU index of eligible scheduling contexts.
// Producer/consumer: only the owning CPU with interrupts disabled.
// Ordering: highest urgency first, FIFO within one urgency.
// Membership: Sc::ready_hook_ is unique and never linked elsewhere.
class ReadyQueue final {
    using Level = libk::IntrusiveList<Sc, &Sc::ready_hook_>;

public:
    [[nodiscard]] auto empty() const noexcept -> bool { return bitmap_ == 0; }
    [[nodiscard]] auto size() const noexcept -> usize { return size_; }

    void enqueue(Sc& sc, Urgency urgency) noexcept;
    void remove(Sc& sc, Urgency urgency) noexcept;
    [[nodiscard]] auto front() noexcept
        -> Sc*;
    [[nodiscard]] auto select() noexcept -> Sc*;
    [[nodiscard]] auto pop_front(Urgency urgency) noexcept -> Sc*;

private:
    Level levels_[Urgency::level_count]{};
    u32 bitmap_{};
    usize size_{};
};

class Dispatcher;

// Fixed-storage one-shot deadline owned by exactly one Dispatcher while
// armed. The callback runs on that CPU with interrupts disabled; remote
// producers publish subsystem state and wake the owner instead of mutating
// this queue.
class Deadline final : private libk::noncopyable_nonmovable {
public:
    using Callback = libk::delegate<void() noexcept>;

    explicit Deadline(Callback callback) noexcept : callback_(callback) {}
    ~Deadline() noexcept;

    [[nodiscard]] auto armed() const noexcept -> bool {
        return owner_ != nullptr;
    }

private:
    friend class Dispatcher;
    friend class DeadlineQueue;

    libk::IntrusiveTreeHook hook_{};
    Callback callback_{};
    Dispatcher* owner_{};
    time::Instant when_{};
};

class DeadlineQueue final {
    struct Earlier final {
        [[nodiscard]] auto operator()(
            const Deadline& lhs,
            const Deadline& rhs) const noexcept -> bool {
            if (lhs.when_ != rhs.when_) {
                return lhs.when_ < rhs.when_;
            }
            return reinterpret_cast<usize>(&lhs)
                < reinterpret_cast<usize>(&rhs);
        }
    };
    using Tree = libk::IntrusiveTree<
        Deadline, &Deadline::hook_, Earlier>;

public:
    [[nodiscard]] auto deadline() const noexcept
        -> std::optional<time::Instant>;
    [[nodiscard]] auto front() noexcept -> Deadline* {
        return tree_.minimum();
    }
    void insert(Deadline& deadline, time::Instant when) noexcept;
    void remove(Deadline& deadline) noexcept;

private:
    Tree tree_{};
};

// Dispatcher-owned ordered index of throttled contexts.
// Only the owning CPU mutates it with interrupts disabled. Ordering is by the
// SC's next refill deadline, then stable context address. Refill amounts remain
// canonical in Sc; this queue stores no budget copy.
class TimerQueue final {
    struct Earlier final {
        [[nodiscard]] auto operator()(
            const Sc& lhs,
            const Sc& rhs) const noexcept -> bool {
            if (lhs.timer_deadline_ != rhs.timer_deadline_) {
                return lhs.timer_deadline_ < rhs.timer_deadline_;
            }
            return reinterpret_cast<usize>(&lhs)
                < reinterpret_cast<usize>(&rhs);
        }
    };
    using Tree = libk::IntrusiveTree<
        Sc, &Sc::timer_hook_, Earlier>;

public:
    [[nodiscard]] auto empty() const noexcept -> bool { return tree_.empty(); }
    [[nodiscard]] auto size() const noexcept -> usize { return tree_.size(); }
    [[nodiscard]] auto deadline() const noexcept
        -> std::optional<time::Instant>;
    [[nodiscard]] auto front() noexcept -> Sc* { return tree_.minimum(); }

    void insert(Sc& sc, time::Instant deadline) noexcept;
    void remove(Sc& sc) noexcept;

private:
    Tree tree_{};
};

} // namespace sched
