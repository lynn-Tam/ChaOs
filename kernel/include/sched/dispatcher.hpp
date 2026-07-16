#pragma once

#include <core/types.hpp>
#include <cpu/topology.hpp>
#include <libk/noncopyable.hpp>
#include <libk/optional.hpp>
#include <sched/builtin_policy.hpp>
#include <sched/timer_queue.hpp>
#include <sched/trace.hpp>
#include <sched/wake_queue.hpp>
#include <sched/types.hpp>
#include <time/clock.hpp>

namespace kernel {
struct CpuLocal;
class CpuRegistry;
class Thread;
}

namespace kernel::sched {

class CpuDispatcher final : private libk::noncopyable_nonmovable {
public:
    enum class WakeError : u8 {
        WrongCpu,
        Unavailable,
    };
    enum class WakeAcceptance : u8 {
        Rejected,
        Accepted,
        Readied,
    };
    using WakeResult = libk::Expected<void, WakeError>;
    CpuDispatcher(
        CpuLocal& cpu,
        CpuId id,
        Thread& idle,
        time::Clock& clock) noexcept;

    [[nodiscard]] auto current() noexcept -> Thread* { return current_; }
    [[nodiscard]] auto current() const noexcept -> const Thread* {
        return current_;
    }
    [[nodiscard]] auto current_binding() noexcept -> Binding* {
        return current_binding_;
    }
    [[nodiscard]] auto ready_count() const noexcept -> usize {
        return policy_.ready_count();
    }

    [[noreturn]] void enter_idle() noexcept;
    void on_context_enter() noexcept;

    [[nodiscard]] auto make_ready(Binding& binding) noexcept -> bool;
    [[nodiscard]] auto accept_wake(Binding& binding) noexcept
        -> WakeAcceptance;
    [[nodiscard]] auto post_wake(Binding& binding) noexcept -> WakeResult;
    void yield() noexcept;
    void block_current() noexcept;
    [[noreturn]] void exit_current() noexcept;
    void request_reschedule(DispatchReason reason) noexcept;
    void on_timer() noexcept;
    void drain_remote_wakes() noexcept;
    void on_trap_exit() noexcept;
    void disable_preemption() noexcept;
    void enable_preemption() noexcept;
    void dump_trace() const noexcept;

private:
    void charge_to(time::Instant now) noexcept;
    void enqueue_or_throttle(Binding& binding, time::Instant now) noexcept;
    void process_timers(time::Instant now) noexcept;
    void dispatch(DispatchReason reason, time::Instant now) noexcept;
    void commit(
        Binding* candidate,
        DispatchReason reason,
        time::Instant now) noexcept;
    void publish(Thread& thread) noexcept;
    void program_deadline(time::Instant now) noexcept;
    void post_switch() noexcept;
    void record_dispatch(
        Thread& outgoing,
        Thread& incoming,
        DispatchReason reason,
        time::Instant now) noexcept;

    CpuLocal* cpu_{};
    CpuId id_{};
    Thread* idle_{};
    time::Clock* clock_{};
    Thread* current_{};
    Binding* current_binding_{};
    time::Instant accounted_at_{};
    time::Duration quantum_{};
    libk::optional<DispatchReason> pending_{};
    usize preempt_depth_{};
    Thread* handoff_outgoing_{};
    BuiltinPolicy policy_{};
    TimerQueue timers_{};
    WakeQueue wakes_{};
    bool timer_available_{};
    bool ipi_available_{};
    time::Duration pending_charge_{};
    time::Instant programmed_deadline_{time::Instant::max()};
    DispatchTrace trace_{};
};

void yield() noexcept;
void block() noexcept;
[[nodiscard]] auto wake(
    CpuRegistry& cpus,
    Binding& binding) noexcept -> CpuDispatcher::WakeResult;
[[noreturn]] void exit_current() noexcept;

} // namespace kernel::sched
