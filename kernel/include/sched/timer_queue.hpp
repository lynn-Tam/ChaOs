#pragma once

#include <core/types.hpp>
#include <libk/delegate.hpp>
#include <libk/intrusive_tree.hpp>
#include <libk/noncopyable.hpp>
#include <sched/binding.hpp>
#include <time/time.hpp>

namespace kernel::sched {

class CpuDispatcher;

// Fixed-storage one-shot deadline owned by exactly one CpuDispatcher while
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
    friend class CpuDispatcher;
    friend class DeadlineQueue;

    libk::IntrusiveTreeHook hook_{};
    Callback callback_{};
    CpuDispatcher* owner_{};
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
        -> libk::optional<time::Instant>;
    [[nodiscard]] auto front() noexcept -> Deadline* {
        return tree_.minimum();
    }
    void insert(Deadline& deadline, time::Instant when) noexcept;
    void remove(Deadline& deadline) noexcept;

private:
    Tree tree_{};
};

// CpuDispatcher-owned ordered index of throttled Bindings.
// Only the owning CPU mutates it with interrupts disabled. Ordering is by the
// SC's next refill deadline, then stable Binding address. Refill amounts remain
// canonical in SchedulingContext; this queue stores no budget copy.
class TimerQueue final {
    struct Earlier final {
        [[nodiscard]] auto operator()(
            const Binding& lhs,
            const Binding& rhs) const noexcept -> bool {
            if (lhs.timer_deadline_ != rhs.timer_deadline_) {
                return lhs.timer_deadline_ < rhs.timer_deadline_;
            }
            return reinterpret_cast<usize>(&lhs)
                < reinterpret_cast<usize>(&rhs);
        }
    };
    using Tree = libk::IntrusiveTree<
        Binding, &Binding::timer_hook_, Earlier>;

public:
    [[nodiscard]] auto empty() const noexcept -> bool { return tree_.empty(); }
    [[nodiscard]] auto size() const noexcept -> usize { return tree_.size(); }
    [[nodiscard]] auto deadline() const noexcept
        -> libk::optional<time::Instant>;
    [[nodiscard]] auto front() noexcept -> Binding* { return tree_.minimum(); }

    void insert(Binding& binding, time::Instant deadline) noexcept;
    void remove(Binding& binding) noexcept;

private:
    Tree tree_{};
};

} // namespace kernel::sched
