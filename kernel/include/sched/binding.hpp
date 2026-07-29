#pragma once

#include <cpu/topology.hpp>
#include <diag/concurrency.hpp>
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
    [[nodiscard]] auto target_reference() const noexcept
        -> libk::Expected<object::ObjectRef, object::ObjectError> {
        return target_.reference();
    }
    [[nodiscard]] auto queued() const noexcept -> bool {
        return ready_hook_.is_linked();
    }
    [[nodiscard]] auto timer_queued() const noexcept -> bool {
        return timer_hook_.is_linked();
    }

    [[nodiscard]] auto actor_key() const noexcept
        -> diag::concurrency::ObservationKey {
        return actor_.key();
    }

    [[nodiscard]] auto actor_ref() noexcept -> diag::concurrency::NodeRef {
        ensure_actor(diag::concurrency::SourceSite::current());
        return diag::concurrency::NodeRef::observation(actor_.key());
    }

    void publish_state(
        ExecutionState state,
        diag::concurrency::SourceSite site =
            diag::concurrency::SourceSite::current()) noexcept {
        ensure_actor(site);
        publish_projection();
        diag::concurrency::record(
            diag::concurrency::FlightDomain::Scheduler,
            state_event(state),
            actor_.key().raw,
            reinterpret_cast<u64>(this),
            static_cast<u64>(state),
            home_cpu_.raw,
            0,
            site);
    }

    void link_wait(
        diag::concurrency::ObservationKey wait,
        diag::concurrency::WaitKind kind,
        diag::concurrency::NodeRef driver,
        diag::concurrency::SourceSite site =
            diag::concurrency::SourceSite::current()) noexcept {
        ensure_actor(site);
        actor_.link_wait(wait, kind, driver, site);
    }

    void clear_wait(
        diag::concurrency::SourceSite site =
            diag::concurrency::SourceSite::current()) noexcept {
        actor_.clear_wait(site);
        publish_actor();
    }

#if MYOS_CONCURRENCY_PROBE == 3 || MYOS_CONCURRENCY_PROBE == 4
    //Confirmatory experiment.
    // Exit condition: remove when operation fault injection no longer borrows
    // a real Ready binding before its first dispatch.
    void suppress_actor_for_probe() noexcept {
        actor_.watch(false);
    }
#endif

private:
    friend class ReadyQueue;
    friend class BuiltinPolicy;
    friend class TimerQueue;
    friend class RemoteQueue;
    friend class CpuDispatcher;

    // Scheduler state remains canonical in Execution/queues.  These bits are
    // only the bounded projection consumed by the wait-graph analyzer:
    // queued, refill-timer queued, retained wake, retained activation.
    void publish_projection() noexcept {
        ensure_actor(diag::concurrency::SourceSite::current());
        u64 projection = (queued() ? 1U : 0U)
            | (timer_queued() ? 2U : 0U)
            | (wake_credit_ ? 4U : 0U)
            | (activation_credit_ ? 8U : 0U);
        actor_.detail(0, projection);
        actor_.detail(1, static_cast<u64>(execution().state()));
        const u64 remote = (start_.pending() ? 1U : 0U)
            | (wake_.pending() ? 2U : 0U)
            | (stop_.pending() ? 4U : 0U);
        actor_.detail(2, remote);
        actor_.detail(3, home_cpu_.raw);
        publish_actor();
    }

    void publish_actor() noexcept {
        using diag::concurrency::Expectation;
        using diag::concurrency::NodeRef;
        using diag::concurrency::WaitKind;

        const ExecutionState state = execution().state();
        const auto site = diag::concurrency::SourceSite::current();
        const auto home = NodeRef::cpu(home_cpu_);
        const u32 phase = static_cast<u32>(state);
        const u64 stamp = static_cast<u64>(state);
        switch (state) {
        case ExecutionState::Ready:
            actor_.deadline(0);
            actor_.transition(
                phase, stamp, WaitKind::SchedulerReady, home, {}, site);
            actor_.watch(true);
            return;
        case ExecutionState::Throttled:
            actor_.deadline(
                timer_queued() ? timer_deadline_.ticks() : 0,
                diag::concurrency::default_grace(
                    Expectation::SchedulerControlled),
                site);
            actor_.transition(
                phase, stamp, WaitKind::SchedulerRefill, home, {}, site);
            actor_.watch(timer_queued());
            return;
        case ExecutionState::Blocked:
            actor_.phase(phase, stamp, site);
            actor_.watch(true);
            return;
        case ExecutionState::Parked:
            actor_.deadline(0);
            actor_.transition(
                phase,
                stamp,
                activation_credit_ ? WaitKind::SchedulerActivation
                                   : WaitKind::None,
                activation_credit_ ? home : NodeRef{},
                {},
                site);
            actor_.watch(activation_credit_);
            return;
        case ExecutionState::Prepared:
        case ExecutionState::Running:
        case ExecutionState::Exited:
            actor_.deadline(0);
            actor_.transition(
                phase, stamp, WaitKind::None, {}, {}, site);
            actor_.watch(false);
            return;
        }
    }

    void ensure_actor(diag::concurrency::SourceSite site) noexcept {
        if (!actor_) {
            actor_ = diag::concurrency::ObservationLease::reserve_on(
                home_cpu_,
                diag::concurrency::RecordKind::ExecutionActor,
                reinterpret_cast<u64>(this),
                1,
                diag::concurrency::Expectation::SchedulerControlled,
                site);
        }
    }

    [[nodiscard]] static auto state_event(ExecutionState state) noexcept
        -> diag::concurrency::FlightEvent {
        using Event = diag::concurrency::FlightEvent;
        switch (state) {
        case ExecutionState::Prepared:
            return Event::Start;
        case ExecutionState::Ready:
            return Event::Ready;
        case ExecutionState::Running:
            return Event::Dispatch;
        case ExecutionState::Throttled:
            return Event::Throttle;
        case ExecutionState::Blocked:
            return Event::Block;
        case ExecutionState::Parked:
            return Event::Park;
        case ExecutionState::Exited:
            return Event::Exit;
        }
        return Event::ObservationDegraded;
    }

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
    diag::concurrency::ObservationLease actor_{};
};

} // namespace kernel::sched
