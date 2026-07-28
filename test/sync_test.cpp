#include <test/test.hpp>

#include <arch/cpu.hpp>
#include <arch/interrupt.hpp>
#include <diag/concurrency.hpp>
#include <libk/manual_lifetime.hpp>
#include <libk/utility.hpp>
#include <sync/irq_lock_guard.hpp>
#include <sync/model.hpp>

namespace {

constinit libk::ManualLifetime<kernel::diag::concurrency::ObservationShard>
    concurrency_test_shard{};
constinit libk::ManualLifetime<kernel::diag::concurrency::FlightRecorder>
    concurrency_test_flight{};

[[nodiscard]] auto test_dep_graph_finds_paths_across_words(
    [[maybe_unused]] const TestContext& context) noexcept -> bool {
    static kernel::sync::DepGraph<70> graph{};
    graph.insert(1, 65);
    graph.insert(65, 69);
    static kernel::sync::DepGraph<70>::Path path{};
    path = {};
    return graph.has(1, 65)
        && graph.has(65, 69)
        && graph.path(1, 69, path)
        && path.size == 3
        && path.nodes[0] == 1
        && path.nodes[1] == 65
        && path.nodes[2] == 69;
}

[[nodiscard]] auto test_dep_graph_rejects_absent_path(
    [[maybe_unused]] const TestContext& context) noexcept -> bool {
    kernel::sync::DepGraph<5> graph{};
    graph.insert(0, 1);
    graph.insert(2, 3);
    kernel::sync::DepGraph<5>::Path path{};
    return !graph.path(1, 0, path) && path.size == 0;
}

[[nodiscard]] auto test_dep_graph_serializes_reverse_edge(
    [[maybe_unused]] const TestContext& context) noexcept -> bool {
    kernel::sync::DepGraph<4> graph{};
    const auto first = graph.check_insert(0, 1);
    const auto reverse = graph.check_insert(1, 0);
    return first.status == kernel::sync::DepStatus::Added
        && reverse.status == kernel::sync::DepStatus::Cycle
        && reverse.path.size == 2
        && reverse.path.nodes[0] == 0
        && reverse.path.nodes[1] == 1;
}

[[nodiscard]] auto test_wait_cycle_requires_stable_closed_stamps(
    [[maybe_unused]] const TestContext& context) noexcept -> bool {
    kernel::sync::WaitStamp candidate[2]{
        {0, 0x1000, 0x1010, 7,
            kernel::sync::OwnerWord::pack(kernel::CpuId{1}, 11)},
        {1, 0x2000, 0x2010, 9,
            kernel::sync::OwnerWord::pack(kernel::CpuId{0}, 13)},
    };
    kernel::sync::WaitStamp observed[2]{candidate[0], candidate[1]};
    if (kernel::sync::validate_wait_cycle(candidate, observed, 2)
        != kernel::sync::WaitCheck::Stable) {
        return false;
    }
    ++observed[1].wait_generation;
    if (kernel::sync::validate_wait_cycle(candidate, observed, 2)
        != kernel::sync::WaitCheck::Changed) {
        return false;
    }
    observed[1] = candidate[1];
    observed[0].owner_word = kernel::sync::OwnerWord::pack(
        kernel::CpuId{3}, 11);
    candidate[0].owner_word = observed[0].owner_word;
    return kernel::sync::validate_wait_cycle(candidate, observed, 2)
        == kernel::sync::WaitCheck::Open;
}

[[nodiscard]] auto test_owner_word_preserves_generation(
    [[maybe_unused]] const TestContext& context) noexcept -> bool {
    constexpr u64 word = kernel::sync::OwnerWord::pack(kernel::CpuId{255}, 77);
    return kernel::sync::OwnerWord::cpu(word) == 255
        && kernel::sync::OwnerWord::generation(word) == 77
        && kernel::sync::OwnerWord::cpu(
            kernel::sync::OwnerWord::none(77)) == kernel::max_cpu_count
        && kernel::sync::OwnerWord::generation(
            kernel::sync::OwnerWord::none(77)) == 77;
}

[[nodiscard]] auto test_source_site_is_captured_at_call(
    [[maybe_unused]] const TestContext& context) noexcept -> bool {
    const u32 expected = static_cast<u32>(__LINE__ + 1);
    const kernel::sync::LockSite site = kernel::sync::LockSite::current();
    return site.file != nullptr
        && site.function != nullptr
        && site.line == expected;
}

[[nodiscard]] auto test_irq_token_move_restores_once(
    [[maybe_unused]] const TestContext& context) noexcept -> bool {
    const bool enabled = arch::interrupts_enabled();
    {
        kernel::sync::IrqToken first{};
        if (arch::interrupts_enabled()) {
            return false;
        }
        kernel::sync::IrqToken second{libk::move(first)};
        if (!second.active() || first.active()) {
            return false;
        }
    }
    return arch::interrupts_enabled() == enabled;
}

[[nodiscard]] auto test_untracked_irq_restore_is_ignored(
    [[maybe_unused]] const TestContext& context) noexcept -> bool {
    // Models a token created before per-CPU diagnostics publication and
    // restored after publication. It never contributed to the tracked depth.
    kernel::sync::irq_restoring({});
    return true;
}

[[nodiscard]] auto test_ordered_pair_and_try_token_own_once(
    [[maybe_unused]] const TestContext& context) noexcept -> bool {
    using PairLock = kernel::sync::SpinLock<
        kernel::sync::LockClass::CSpace,
        kernel::sync::SameClassPolicy::AddressAscending>;
    PairLock first{};
    PairLock second{};
    {
        kernel::sync::OrderedIrqLockPair pair{first, second};
    }

    kernel::sync::SpinLock<kernel::sync::LockClass::Wait> try_target{};
    kernel::sync::IrqLockToken token{
        try_target, kernel::sync::try_lock};
    return token.owns_lock();
}

[[maybe_unused, nodiscard]] auto test_observation_key_generation_and_snapshot(
    [[maybe_unused]] const TestContext& context) noexcept -> bool {
    using namespace kernel::diag::concurrency;
    bool result = false;
    {
        ObservationShard& shard = concurrency_test_shard.emplace();
        shard.initialize(kernel::CpuId{3});
        ObservationLease lease = shard.reserve(
            RecordKind::Operation,
            0x1234,
            9,
            Expectation::InternalFinite,
            SourceSite::current());
        do {
            if (!lease || lease.key().shard() != kernel::CpuId{3}
                || lease.key().slot() >= ObservationShard::slot_count) {
                break;
            }
            const ObservationKey first = lease.key();
            lease.transition(
                7,
                11,
                WaitKind::OperationCompletion,
                NodeRef::cpu(kernel::CpuId{1}),
                NodeRef::external(0x55));
            lease.watch(true);
            ObservationSnapshot snapshot{};
            if (!lease.snapshot(snapshot)
                || snapshot.generation != first.generation()
                || snapshot.phase != 7
                || snapshot.semantic_stamp != 11
                || snapshot.driver.kind != NodeRef::Kind::Cpu
                || snapshot.blocker.kind != NodeRef::Kind::External
                || (shard.watched() & (u64{1} << first.slot())) == 0) {
                break;
            }
            lease.finish(8, 0xdead);
            const ObservationKey second = shard.reserve(
                RecordKind::Operation,
                0x1234,
                9,
                Expectation::InternalFinite,
                SourceSite::current()).key();
            result = second && second.slot() == first.slot()
                && second.generation() == first.generation() + 1
                && second != first;
        } while (false);
    }
    concurrency_test_shard.reset();
    return result;
}

[[maybe_unused, nodiscard]] auto test_observation_activity_is_not_progress(
    [[maybe_unused]] const TestContext& context) noexcept -> bool {
    using namespace kernel::diag::concurrency;
    bool result = false;
    {
        ObservationShard& shard = concurrency_test_shard.emplace();
        shard.initialize(kernel::CpuId{0});
        ObservationLease lease = shard.reserve(
            RecordKind::ServiceWork,
            0,
            0,
            Expectation::InternalFinite,
            SourceSite::current());
        do {
            if (!lease) {
                break;
            }
            ObservationSnapshot before{};
            if (!lease.snapshot(before)) {
                break;
            }
            lease.attempt(
                1,
                WaitKind::Unknown,
                NodeRef::cpu(kernel::CpuId{0}));
            ObservationSnapshot attempted{};
            if (!lease.snapshot(attempted)
                || attempted.activity_epoch <= before.activity_epoch
                || attempted.progress_epoch != before.progress_epoch) {
                break;
            }
            lease.transition(
                1,
                42,
                WaitKind::Unknown,
                NodeRef::cpu(kernel::CpuId{0}));
            ObservationSnapshot progressed{};
            if (!lease.snapshot(progressed)
                || progressed.progress_epoch <= attempted.progress_epoch) {
                break;
            }
            lease.transition(
                1,
                42,
                WaitKind::Unknown,
                NodeRef::cpu(kernel::CpuId{0}));
            ObservationSnapshot stable{};
            const bool stable_read = lease.snapshot(stable);
            result = stable_read
                && stable.progress_epoch == progressed.progress_epoch
                && stable.activity_epoch > progressed.activity_epoch;
        } while (false);
    }
    concurrency_test_shard.reset();
    return result;
}

[[maybe_unused, nodiscard]] auto test_flight_recorder_is_bounded_and_readable(
    [[maybe_unused]] const TestContext& context) noexcept -> bool {
    using namespace kernel::diag::concurrency;
    bool result = false;
    {
        FlightRecorder& flight = concurrency_test_flight.emplace();
        flight.initialize(kernel::CpuId{1});
        for (usize index = 0; index < FlightRecorder::capacity + 3; ++index) {
            flight.push(
                index,
                FlightDomain::Scheduler,
                FlightEvent::Dispatch,
                index,
                index + 1);
        }
        if (flight.head() == FlightRecorder::capacity + 3
            && flight.wrapped()) {
            FlightRecordValue record{};
            result = flight.read(0, record)
                && record.actor == 3
                && record.subject == 4
                && !flight.read(FlightRecorder::capacity, record);
        }
    }
    concurrency_test_flight.reset();
    return result;
}

[[maybe_unused, nodiscard]] auto test_wait_graph_is_bounded_and_generation_checked(
    [[maybe_unused]] const TestContext& context) noexcept -> bool {
    using namespace kernel::diag::concurrency;
    bool result = false;
    {
        ObservationShard& shard = concurrency_test_shard.emplace();
        shard.initialize(kernel::CpuId{3});
        ObservationLease actor = shard.reserve(
            RecordKind::ExecutionActor,
            0x10,
            1,
            Expectation::InternalFinite,
            SourceSite::current());
        ObservationLease operation = shard.reserve(
            RecordKind::Operation,
            0x20,
            1,
            Expectation::InternalFinite,
            SourceSite::current());
        do {
            if (!actor || !operation) {
                break;
            }
            actor.link_wait(
                operation.key(),
                WaitKind::OperationCompletion,
                NodeRef::observation(actor.key()));
            operation.transition(
                1,
                1,
                WaitKind::CompletionPublication,
                NodeRef::observation(actor.key()));
            actor.watch(true);
            operation.watch(true);
            WaitGraphScratch scratch{};
            if (!analyze(actor.key(), scratch)
                || scratch.classification != StallClass::DeadlockCycle
                || scratch.count != 2) {
                break;
            }

            // Canonical ExecutionState::Blocked and Completion::Ready are
            // correlated only for diagnosis; the analyzer must not repair
            // the scheduler's wake credit.
            actor.phase(4, 4);
            operation.phase(3, 3);
            scratch = {};
            if (!analyze(actor.key(), scratch)
                || scratch.classification != StallClass::LostWake) {
                break;
            }

            operation.finish(2);
            actor.clear_wait();
            actor.finish(2);
            ObservationLease orphan = shard.reserve(
                RecordKind::ServiceWork,
                0x30,
                1,
                Expectation::InternalFinite,
                SourceSite::current());
            if (!orphan) {
                break;
            }
            scratch = {};
            result = analyze(orphan.key(), scratch)
                && scratch.classification == StallClass::OrphanObligation;
            orphan.finish(1);
        } while (false);
    }
    concurrency_test_shard.reset();
    return result;
}

} // namespace

void register_sync_tests(TestRegistry& registry) noexcept {
    (void)registry.add(
        "sync",
        "dependency graph spans multiword rows and reconstructs paths",
        test_dep_graph_finds_paths_across_words);
    (void)registry.add(
        "sync",
        "dependency graph leaves absent paths empty",
        test_dep_graph_rejects_absent_path);
    (void)registry.add(
        "sync",
        "serialized reverse edge returns a typed cycle",
        test_dep_graph_serializes_reverse_edge);
    (void)registry.add(
        "sync",
        "wait-cycle validation rejects changed and open snapshots",
        test_wait_cycle_requires_stable_closed_stamps);
    (void)registry.add(
        "sync",
        "owner word preserves CPU and acquisition generation",
        test_owner_word_preserves_generation);
    (void)registry.add(
        "sync",
        "LockSite compiler builtins capture the call site",
        test_source_site_is_captured_at_call);
    (void)registry.add(
        "sync",
        "movable IRQ ownership restores the original state once",
        test_irq_token_move_restores_once);
    (void)registry.add(
        "sync",
        "untracked early IRQ ownership does not consume tracked depth",
        test_untracked_irq_restore_is_ignored);
    (void)registry.add(
        "sync",
        "ordered pair and try token own successful acquisitions once",
        test_ordered_pair_and_try_token_own_once);
#if MYOS_CONCURRENCY_DIAG >= 1
    (void)registry.add(
        "concurrency",
        "observation keys reject reuse and publish stable snapshots",
        test_observation_key_generation_and_snapshot);
    (void)registry.add(
        "concurrency",
        "activity attempts do not masquerade as semantic progress",
        test_observation_activity_is_not_progress);
    (void)registry.add(
        "concurrency",
        "bounded wait graph detects cycles and orphan obligations",
        test_wait_graph_is_bounded_and_generation_checked);
#endif
#if MYOS_CONCURRENCY_DIAG >= 2
    (void)registry.add(
        "concurrency",
        "flight recorder wraps without losing the newest history",
        test_flight_recorder_is_bounded_and_readable);
#endif
}
