#pragma once

#include <expected>
#include <array>
#include <utility>


#include <libk/types.hpp>
#include <cpu/types.hpp>
#include <task/thread.hpp>
#include <libk/noncopyable.hpp>
#include <optional>
#include <limits>
#include <sched/sc.hpp>
#include <sched/sched.hpp>
#include <time.hpp>
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
    [[nodiscard]] auto remaining_budget() const noexcept -> u64;

    [[nodiscard]] auto arm(
        Deadline& deadline,
        u64 when) noexcept -> bool;
    void disarm(Deadline& deadline) noexcept;

    [[noreturn]] void enter_idle() noexcept;
    void on_context_enter() noexcept;

    void refresh(Thread&) noexcept;
    void request_control(Thread&) noexcept;
    [[nodiscard]] auto accept_wake(Thread&) noexcept -> WakeAcceptance;
    [[nodiscard]] auto post_wake(Thread&) noexcept -> WakeResult;
    void request_stop(Thread&) noexcept;
    void drain_remote() noexcept;
    void post(Domain::Local&) noexcept;
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
    void resched() noexcept;

private:
    friend class Sc;
    friend struct Domain::Local;
    enum Action : u8 { Control = 1, Wake = 2, Stop = 4 };
    enum class StopDisposition : u8 {
        Deferred,
        Finalize,
    };

    void enqueue(Thread&) noexcept;
    void remove(Thread&) noexcept;
    Thread* select(u64) noexcept;
    void detach(Thread&) noexcept;
    auto maintenance(const Thread&) const noexcept -> bool;
    void charge_to(u64 now) noexcept;

    void insert(Deadline&, u64) noexcept;
    void expire(u64 now) noexcept;
    void dispatch(
        DispatchReason reason,
        u64 now,
        status_t exit_status = STATUS_OK) noexcept;
    void commit(
        Thread* candidate,
        DispatchReason reason,
        u64 now,
        status_t exit_status) noexcept;
    void publish(Thread* target) noexcept;
    void set_timer(u64 now) noexcept;
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
        u64 now) noexcept;
    void post(Thread&, u8 actions) noexcept;
    void complete(Thread&) noexcept;
    auto migrate(Thread&) noexcept -> bool;
    object::ref<> departure_;

    [[nodiscard]] auto kick_remote() noexcept -> WakeResult;

    Cpu& cpu_;
    Thread* idle_{};
    time::Clock* clock_{};
    Sc* current_sc_{};
    Node* current_branch_{};
    Budget::Loan loan_{};
    struct Ticket { Domain* domain; Budget::Loan loan; };
    std::array<Ticket, Domain::max_depth> tickets_{};
    usize ticket_count_{};
    u64 duration_{};
    auto reserve(Thread&, u64) noexcept -> bool;
    void settle(u64) noexcept;
    void drain_domains() noexcept;
    u64 loan_elapsed_{};
    u64 accounted_at_{};
    u64 quantum_{};
    u64 run_until_{std::numeric_limits<u64>::max()};
    std::optional<DispatchReason> pending_{};
    status_t pending_exit_status_{STATUS_OK};
    Thread* handoff_outgoing_{};
    DispatchReason handoff_reason_{DispatchReason::Exit};
    status_t handoff_exit_status_{STATUS_OK};
    Node::Queue ready_;
    void insert(Node&) noexcept;
    void erase(Node&) noexcept;
    void domain_ready(Domain::Local&) noexcept;
    libk::intrusive_list<Thread, &Thread::maintenance_hook_> maintenance_ready_{};
    Sc maintenance_;
    std::atomic<u64> fair_{};
    u64 enqueue_seq_{};
    usize ready_count_{};
    struct Earlier {
        bool operator()(const auto& a, const auto& b) const noexcept {
            return std::pair{a.when_, reinterpret_cast<usize>(&a)}
                 < std::pair{b.when_, reinterpret_cast<usize>(&b)};
        }
    };
    libk::intrusive_tree<Deadline, &Deadline::hook_, Earlier> deadlines_{};
    sync::Spin mail_lock_{};
    libk::intrusive_list<Thread, &Thread::mail_hook_> mail_{};
    libk::intrusive_list<Domain::Local, &Domain::Local::mail_hook> domains_{};
    bool has_timer_{};
    bool has_ipi_{};
};

void yield() noexcept;
void block() noexcept;
[[nodiscard]] auto wake(Cpus&, Thread&) noexcept -> Dispatcher::WakeResult;
[[noreturn]] void exit_current() noexcept;

} // namespace sched
