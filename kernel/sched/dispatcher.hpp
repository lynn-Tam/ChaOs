#pragma once

#include <expected>
#include <array>
#include <utility>


#include <base/types.hpp>
#include <cpu/types.hpp>
#include <task/thread.hpp>
#include <libk/noncopyable.hpp>
#include <optional>
#include <sched/sc.hpp>
#include <sched/sched.hpp>
#include <time/clock.hpp>
#include <uapi/abi.h>

struct Cpu;
class Cpus;
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
        Cpu& cpu,
        Thread& idle,
        time::Clock& clock) noexcept;
    ~Dispatcher() noexcept = default;

    auto clock() noexcept -> time::Clock& { return *clock_; }
    [[nodiscard]] auto current() const noexcept -> Thread*;
    [[nodiscard]] auto current_sc() noexcept -> Sc* {
        return current_sc_;
    }
    [[nodiscard]] auto id() const noexcept -> CpuId;
    [[nodiscard]] auto ready_count() const noexcept -> usize {
        return ready_count_;
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
        status_t exit_status) noexcept;
    void on_timer() noexcept;
    void on_trap_exit() noexcept;
    // Re-publishes the current Thread's derived effective stack and roots
    // after an Activation push/pop. It does not change target or SC ownership.
    void refresh() noexcept;
    void disable_preemption() noexcept;
    void enable_preemption() noexcept;

private:
    friend class Sc;
    enum Action : u8 { Start = 1, Wake = 2, Stop = 4 };
    void cancel(Sc&) noexcept;
    enum class StopDisposition : u8 {
        Deferred,
        Finalize,
    };

    void enqueue(Sc&) noexcept;
    void remove(Sc&) noexcept;
    Sc* select() noexcept;
    void charge_to(time::Instant now) noexcept;
    void enqueue_or_throttle(Sc& sc, time::Instant now) noexcept;
    void process_timers(time::Instant now) noexcept;
    void process_deadlines(time::Instant now) noexcept;
    void dispatch(
        DispatchReason reason,
        time::Instant now,
        status_t exit_status = STATUS_OK) noexcept;
    void commit(
        Sc* candidate,
        DispatchReason reason,
        time::Instant now,
        status_t exit_status) noexcept;
    void publish(Thread* target) noexcept;
    void program_deadline(time::Instant now) noexcept;
    void post_switch() noexcept;
    [[nodiscard]] auto stop(Thread* target) noexcept
        -> StopDisposition;
    void finish_exit(
        Thread* target,
        DispatchReason reason = DispatchReason::Exit,
        status_t exit_status = STATUS_OK) noexcept;
    void record_dispatch(
        Thread* outgoing,
        Thread* incoming,
        DispatchReason reason,
        time::Instant now) noexcept;
    void post(Sc&, u8 actions) noexcept;
    void complete(Sc&) noexcept;
    [[nodiscard]] auto post_remote(Sc&, u8 actions) noexcept -> WakeResult;

    [[nodiscard]] auto kick_remote() noexcept -> WakeResult;

    Cpu& cpu_;
    Thread* idle_{};
    time::Clock* clock_{};
    Sc* current_sc_{};
    time::Instant accounted_at_{};
    time::Duration quantum_{};
    std::optional<DispatchReason> pending_{};
    status_t pending_exit_status_{STATUS_OK};
    usize preempt_depth_{};
    Thread* handoff_outgoing_{};
    DispatchReason handoff_reason_{DispatchReason::Exit};
    status_t handoff_exit_status_{STATUS_OK};
    using Level = libk::IntrusiveList<Sc, &Sc::ready_hook_>;
    std::array<Level, Urgency::level_count> ready_{};
    u32 ready_mask_{};
    usize ready_count_{};
    template<auto Stamp> struct Earlier {
        bool operator()(const auto& a, const auto& b) const noexcept {
            return std::pair{Stamp(a), reinterpret_cast<usize>(&a)}
                 < std::pair{Stamp(b), reinterpret_cast<usize>(&b)};
        }
    };
    // A throttled ledger is immutable while indexed; no deadline mirror.
    libk::IntrusiveTree<Sc, &Sc::timer_hook_, Earlier<
        [](const Sc& sc) { return sc.next_refill(); }>> timers_{};
    libk::IntrusiveTree<Deadline, &Deadline::hook_, Earlier<
        [](const Deadline& d) { return d.when_; }>> deadlines_{};
    sync::Spin mail_lock_{};
    libk::IntrusiveList<Sc, &Sc::mail_hook_> mail_{};
    bool timer_available_{};
    bool ipi_available_{};
    time::Instant programmed_deadline_{time::Instant::max()};
};

void yield() noexcept;
void block() noexcept;
[[nodiscard]] auto wake(
    Cpus& cpus,
    Sc& sc) noexcept
    -> Dispatcher::WakeResult;

[[nodiscard]] auto start(
    Cpus& cpus,
    Sc& sc) noexcept -> Dispatcher::WakeResult;
[[noreturn]] void exit_current() noexcept;

} // namespace sched
