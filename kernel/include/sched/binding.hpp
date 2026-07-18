#pragma once

#include <cpu/topology.hpp>
#include <execution/target.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/intrusive_tree.hpp>
#include <libk/noncopyable.hpp>
#include <sched/remote_queue.hpp>
#include <time/time.hpp>

namespace kernel::sched {

class SchedulingContext;
class CpuDispatcher;
class ReadyQueue;
class TimerQueue;

// Canonical relation between one consumable SchedulingContext and one closed
// execution target. TargetHold is the sole target lifetime owner.
class Binding final : private libk::noncopyable_nonmovable {
public:
    Binding(
        SchedulingContext& context,
        execution::TargetHold&& target,
        CpuId home_cpu) noexcept
        : context_(&context),
          target_(libk::move(target)),
          home_cpu_(home_cpu),
          start_(RemoteKind::Start, this),
          wake_(RemoteKind::Wake, this) {}

    [[nodiscard]] auto context() noexcept -> SchedulingContext& {
        return *context_;
    }
    [[nodiscard]] auto context() const noexcept -> const SchedulingContext& {
        return *context_;
    }
    [[nodiscard]] auto target() noexcept -> execution::Target {
        return target_.get();
    }
    [[nodiscard]] auto target() const noexcept -> execution::Target {
        return target_.get();
    }
    [[nodiscard]] auto execution() noexcept -> kernel::Execution& {
        return target().execution();
    }
    [[nodiscard]] auto execution() const noexcept -> const kernel::Execution& {
        return target().execution();
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
    friend class RemoteQueue;
    friend class CpuDispatcher;

    SchedulingContext* context_{};
    execution::TargetHold target_{};
    CpuId home_cpu_{};
    libk::IntrusiveListHook ready_hook_{};
    libk::IntrusiveTreeHook timer_hook_{};
    time::Instant timer_deadline_{};
    RemoteRequest start_;
    RemoteRequest wake_;
    RemoteRequest stop_{RemoteKind::Stop, this};
    // Home-CPU-owned one-bit event credit. It closes the wake-before-block
    // race without allowing a remote producer to modify Thread state.
    bool wake_credit_{};
    bool activation_credit_{};
};

} // namespace kernel::sched
