#pragma once

#include <core/types.hpp>
#include <cpu/topology.hpp>
#include <execution/target.hpp>
#include <libk/noncopyable.hpp>
#include <libk/optional.hpp>
#include <sched/builtin_policy.hpp>
#include <sched/timer_queue.hpp>
#include <sched/trace.hpp>
#include <sched/remote_queue.hpp>
#include <sched/types.hpp>
#include <time/clock.hpp>

namespace kernel {
struct CpuLocal;
class CpuRegistry;
class Thread;
class Vproc;
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

    [[nodiscard]] auto current() const noexcept -> execution::Target {
        return current_;
    }
    [[nodiscard]] auto current_binding() noexcept -> Binding* {
        return current_binding_;
    }
    [[nodiscard]] auto id() const noexcept -> CpuId { return id_; }
    [[nodiscard]] auto ready_count() const noexcept -> usize {
        return policy_.ready_count();
    }
    [[nodiscard]] auto remaining_budget() const noexcept -> time::Duration;
    [[nodiscard]] auto current_urgency() const noexcept -> Urgency;

    [[noreturn]] void enter_idle() noexcept;
    void on_context_enter() noexcept;

    [[nodiscard]] auto make_ready(Binding& binding) noexcept -> bool;
    [[nodiscard]] auto accept_wake(Binding& binding) noexcept
        -> WakeAcceptance;
    [[nodiscard]] auto post_wake(Binding& binding) noexcept -> WakeResult;
    [[nodiscard]] auto post_start(Binding& binding) noexcept -> WakeResult;
    [[nodiscard]] auto accept_activation(Vproc& vproc) noexcept -> bool;
    [[nodiscard]] auto post_activation(Vproc& vproc) noexcept -> WakeResult;
    [[nodiscard]] static auto request_activation(
        CpuRegistry& cpus,
        Vproc& vproc) noexcept -> WakeResult;
    void request_stop(Thread& thread) noexcept;
    void request_stop(Vproc& vproc) noexcept;
    void drain_remote() noexcept;
    void yield() noexcept;
    void block_current() noexcept;
    void park_current() noexcept;
    [[noreturn]] void exit_current() noexcept;
    void request_reschedule(DispatchReason reason) noexcept;
    void on_timer() noexcept;
    void on_trap_exit() noexcept;
    // Re-publishes the current Execution's derived effective stack and roots
    // after an Activation push/pop. It does not change target or SC ownership.
    void refresh() noexcept;
    void disable_preemption() noexcept;
    void enable_preemption() noexcept;
    void dump_trace() const noexcept;

private:
    enum class StopDisposition : u8 {
        Deferred,
        Finalize,
    };

    void charge_to(time::Instant now) noexcept;
    void enqueue_or_throttle(Binding& binding, time::Instant now) noexcept;
    void process_timers(time::Instant now) noexcept;
    void dispatch(DispatchReason reason, time::Instant now) noexcept;
    void commit(
        Binding* candidate,
        DispatchReason reason,
        time::Instant now) noexcept;
    void publish(execution::Target target) noexcept;
    void program_deadline(time::Instant now) noexcept;
    void post_switch() noexcept;
    [[nodiscard]] auto stop(execution::Target target) noexcept
        -> StopDisposition;
    void finish_exit(execution::Target target) noexcept;
    void record_dispatch(
        execution::Target outgoing,
        execution::Target incoming,
        DispatchReason reason,
        time::Instant now) noexcept;
    [[nodiscard]] auto post_remote(RemoteRequest& request) noexcept
        -> WakeResult;
    void request_stop(execution::Target target) noexcept;

    CpuLocal* cpu_{};
    CpuId id_{};
    Thread* idle_{};
    time::Clock* clock_{};
    execution::Target current_{};
    Binding* current_binding_{};
    time::Instant accounted_at_{};
    time::Duration quantum_{};
    libk::optional<DispatchReason> pending_{};
    usize preempt_depth_{};
    execution::Target handoff_outgoing_{};
    BuiltinPolicy policy_{};
    TimerQueue timers_{};
    RemoteQueue remote_{};
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
[[nodiscard]] auto start(
    CpuRegistry& cpus,
    Binding& binding) noexcept -> CpuDispatcher::WakeResult;
[[nodiscard]] auto activate(CpuRegistry& cpus, Vproc& vproc) noexcept
    -> CpuDispatcher::WakeResult;
[[noreturn]] void exit_current() noexcept;

} // namespace kernel::sched
