#pragma once

#include <expected>


#include <base/types.hpp>
#include <cpu/types.hpp>
#include <task/thread.hpp>
#include <libk/noncopyable.hpp>
#include <optional>
#include <sched/queues.hpp>
#include <sched/remote_queue.hpp>
#include <sched/types.hpp>
#include <time/clock.hpp>
#include <uapi/status.h>

struct CpuLocal;
class CpuRegistry;
class Thread;

namespace sched {

class Dispatcher final : private libk::noncopyable_nonmovable {
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
    using WakeResult = std::expected<void, WakeError>;
    Dispatcher(
        CpuLocal& cpu,
        CpuId id,
        Thread& idle,
        time::Clock& clock) noexcept;
    ~Dispatcher() noexcept;

    [[nodiscard]] auto current() const noexcept -> Thread*;
    [[nodiscard]] auto current_sc() noexcept -> Sc* {
        return current_sc_;
    }
    [[nodiscard]] auto id() const noexcept -> CpuId { return id_; }
    [[nodiscard]] auto ready_count() const noexcept -> usize {
        return ready_.size();
    }
    [[nodiscard]] auto remaining_budget() const noexcept -> time::Duration;
    [[nodiscard]] auto current_urgency() const noexcept -> Urgency;
    [[nodiscard]] auto arm(
        Deadline& deadline,
        time::Instant when) noexcept -> bool;
    void disarm(Deadline& deadline) noexcept;

    [[noreturn]] void enter_idle() noexcept;
    void on_context_enter() noexcept;

    [[nodiscard]] auto make_ready(Sc& sc) noexcept -> bool;
    [[nodiscard]] auto accept_wake(
        Sc& sc) noexcept
        -> WakeAcceptance;
    [[nodiscard]] auto post_wake(
        Sc& sc) noexcept
        -> WakeResult;
    [[nodiscard]] auto post_start(Sc& sc) noexcept -> WakeResult;
    void request_stop(Thread&) noexcept;
    void drain_remote() noexcept;
    void yield() noexcept;
    void block_current() noexcept;
    [[noreturn]] void exit_current() noexcept;
    void request_reschedule(DispatchReason reason) noexcept;
    // Exit status is carried alongside the scheduler handoff until the
    // target's single terminal claim. Other dispatch reasons ignore it.
    void request_reschedule(
        DispatchReason reason,
        myos_status_t exit_status) noexcept;
    void on_timer() noexcept;
    void on_trap_exit() noexcept;
    // Re-publishes the current Thread's derived effective stack and roots
    // after an Activation push/pop. It does not change target or SC ownership.
    void refresh() noexcept;
    void disable_preemption() noexcept;
    void enable_preemption() noexcept;

private:
    friend class Sc;
    void cancel(Sc&) noexcept;
    enum class StopDisposition : u8 {
        Deferred,
        Finalize,
    };

    void charge_to(time::Instant now) noexcept;
    void enqueue_or_throttle(Sc& sc, time::Instant now) noexcept;
    void process_timers(time::Instant now) noexcept;
    void process_deadlines(time::Instant now) noexcept;
    void dispatch(
        DispatchReason reason,
        time::Instant now,
        myos_status_t exit_status = MYOS_STATUS_OK) noexcept;
    void commit(
        Sc* candidate,
        DispatchReason reason,
        time::Instant now,
        myos_status_t exit_status) noexcept;
    void publish(Thread* target) noexcept;
    void program_deadline(time::Instant now) noexcept;
    void post_switch() noexcept;
    [[nodiscard]] auto stop(Thread* target) noexcept
        -> StopDisposition;
    void finish_exit(
        Thread* target,
        DispatchReason reason = DispatchReason::Exit,
        myos_status_t exit_status = MYOS_STATUS_OK) noexcept;
    void record_dispatch(
        Thread* outgoing,
        Thread* incoming,
        DispatchReason reason,
        time::Instant now) noexcept;
    [[nodiscard]] auto post_remote(
        RemoteRequest& request) noexcept
        -> WakeResult;

    [[nodiscard]] auto kick_remote() noexcept -> WakeResult;

    CpuLocal* cpu_{};
    CpuId id_{};
    Thread* idle_{};
    time::Clock* clock_{};
    Sc* current_sc_{};
    time::Instant accounted_at_{};
    time::Duration quantum_{};
    std::optional<DispatchReason> pending_{};
    myos_status_t pending_exit_status_{MYOS_STATUS_OK};
    usize preempt_depth_{};
    Thread* handoff_outgoing_{};
    DispatchReason handoff_reason_{DispatchReason::Exit};
    myos_status_t handoff_exit_status_{MYOS_STATUS_OK};
    ReadyQueue ready_{};
    TimerQueue timers_{};
    DeadlineQueue deadlines_{};
    RemoteQueue remote_;
    bool timer_available_{};
    bool ipi_available_{};
    time::Instant programmed_deadline_{time::Instant::max()};
};

void yield() noexcept;
void block() noexcept;
[[nodiscard]] auto wake(
    CpuRegistry& cpus,
    Sc& sc) noexcept
    -> Dispatcher::WakeResult;

[[nodiscard]] auto start(
    CpuRegistry& cpus,
    Sc& sc) noexcept -> Dispatcher::WakeResult;
[[noreturn]] void exit_current() noexcept;

} // namespace sched
