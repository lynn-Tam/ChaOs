#pragma once

#include <core/debug.hpp>
#include <core/types.hpp>
#include <libk/intrusive_list.hpp>
#include <sched/binding.hpp>
#include <sched/types.hpp>

namespace kernel::sched {

// BuiltinPolicy-owned canonical set of eligible local Bindings.
// Producer/consumer: only the owning CPU with interrupts disabled.
// Ordering: highest urgency first, FIFO within one urgency.
// Membership: Binding::ready_hook_ is unique and never linked elsewhere.
class ReadyQueue final {
    using Level = libk::IntrusiveList<Binding, &Binding::ready_hook_>;

public:
    [[nodiscard]] auto empty() const noexcept -> bool { return bitmap_ == 0; }
    [[nodiscard]] auto size() const noexcept -> usize { return size_; }

    void enqueue(Binding& binding, Urgency urgency) noexcept;
    void remove(Binding& binding, Urgency urgency) noexcept;
    [[nodiscard]] auto front(bool prefer_activation = true) noexcept
        -> Binding*;
    [[nodiscard]] auto pop_front(Urgency urgency) noexcept -> Binding*;

private:
    Level levels_[Urgency::level_count]{};
    u32 bitmap_{};
    usize size_{};
};

} // namespace kernel::sched
