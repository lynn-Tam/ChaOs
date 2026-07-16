#pragma once

#include <cpu/topology.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/intrusive_tree.hpp>
#include <libk/noncopyable.hpp>
#include <object/object_ref.hpp>
#include <time/time.hpp>

namespace kernel {
class Thread;
}

namespace kernel::sched {

class SchedulingContext;
class CpuDispatcher;
class ReadyQueue;
class TimerQueue;

// Canonical relation between one consumable SchedulingContext and one Thread.
// The ready hook is meaningful only while the owning Thread is Ready.
class Binding final : private libk::noncopyable_nonmovable {
public:
    Binding(
        SchedulingContext& context,
        object::ObjectHold<Thread>&& target,
        CpuId home_cpu) noexcept
        : context_(&context),
          target_(libk::move(target)),
          home_cpu_(home_cpu) {}

    [[nodiscard]] auto context() noexcept -> SchedulingContext& {
        return *context_;
    }
    [[nodiscard]] auto context() const noexcept -> const SchedulingContext& {
        return *context_;
    }
    [[nodiscard]] auto thread() noexcept -> Thread& { return target_.get(); }
    [[nodiscard]] auto thread() const noexcept -> const Thread& {
        return target_.get();
    }
    [[nodiscard]] auto home_cpu() const noexcept -> CpuId {
        return home_cpu_;
    }
    [[nodiscard]] auto queued() const noexcept -> bool {
        return ready_hook_.is_linked();
    }
    [[nodiscard]] auto timer_queued() const noexcept -> bool {
        return timer_hook_.is_linked();
    }

private:
    friend class ReadyQueue;
    friend class TimerQueue;
    friend class WakeQueue;
    friend class CpuDispatcher;

    SchedulingContext* context_{};
    object::ObjectHold<Thread> target_{};
    CpuId home_cpu_{};
    libk::IntrusiveListHook ready_hook_{};
    libk::IntrusiveTreeHook timer_hook_{};
    time::Instant timer_deadline_{};
    libk::IntrusiveListHook wake_hook_{};
    bool wake_pending_{};
    // Home-CPU-owned one-bit event credit. It closes the wake-before-block
    // race without allowing a remote producer to modify Thread state.
    bool wake_credit_{};
};

} // namespace kernel::sched
