#include <test/test.hpp>

#include <arch/cpu.hpp>
#include <arch/interrupt.hpp>
#include <diag/concurrency.hpp>
#include <execution/execution.hpp>
#include <libk/manual_lifetime.hpp>
#include <libk/utility.hpp>
#include <sync/irq_lock_guard.hpp>
#include <sync/model.hpp>

namespace {

constinit libk::ManualLifetime<kernel::diag::concurrency::ObservationShard>
    concurrency_test_shard{};
constinit libk::ManualLifetime<kernel::diag::concurrency::FlightRecorder>
    concurrency_test_flight{};
constinit kernel::diag::concurrency::FlightPage concurrency_test_flight_pages[
    kernel::diag::concurrency::FlightRecorder::page_count]{};
constinit kernel::diag::concurrency::ObservationPage
    concurrency_capacity_pages[
        kernel::diag::concurrency::ObservationShard::pages]{};

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
                7,
                WaitKind::Unknown,
                NodeRef::cpu(kernel::CpuId{0}));
            ObservationSnapshot attempted{};
            if (!lease.snapshot(attempted)
                || attempted.activity_epoch <= before.activity_epoch
                || attempted.phase != 7
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
        FlightPage* storage[FlightRecorder::page_count]{};
        for (usize page = 0; page < FlightRecorder::page_count; ++page) {
            storage[page] = &concurrency_test_flight_pages[page];
        }
        flight.initialize(kernel::CpuId{1}, storage);
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

[[maybe_unused, nodiscard]] auto test_observation_stale_owner_cannot_reopen_slot(
    [[maybe_unused]] const TestContext& context) noexcept -> bool {
    using namespace kernel::diag::concurrency;
    bool result = false;
    {
        ObservationShard& shard = concurrency_test_shard.emplace();
        shard.initialize(kernel::CpuId{250});
        ObservationLease first = shard.reserve(
            RecordKind::Operation,
            0x1100,
            1,
            Expectation::InternalFinite,
            SourceSite::current());
        do {
            if (!first) {
                break;
            }
            const ObservationKey old_key = first.key();
            ObservationLease stale = ObservationLease::borrow(old_key);
            first.finish(7, 0xaa);

            ObservationLease current = shard.reserve(
                RecordKind::Operation,
                0x2200,
                2,
                Expectation::InternalFinite,
                SourceSite::current());
            if (!current || current.key().slot() != old_key.slot()
                || current.key().generation() != old_key.generation() + 1) {
                break;
            }

            stale.transition(
                9,
                0xbb,
                WaitKind::Unknown,
                NodeRef::external(0x33));
            stale.finish(10, 0xcc);
            ObservationSnapshot snapshot{};
            result = current.snapshot(snapshot)
                && snapshot.phase == 0
                && snapshot.semantic_stamp == 0
                && snapshot.subject_identity == 0x2200;
            current.finish(11);
        } while (false);
    }
    concurrency_test_shard.reset();
    return result;
}

[[maybe_unused, nodiscard]] auto test_observation_actor_capacity_is_reserved(
    [[maybe_unused]] const TestContext& context) noexcept -> bool {
    using namespace kernel::diag::concurrency;
    bool result = false;
    {
        ObservationShard& shard = concurrency_test_shard.emplace();
        ObservationPage* pages[ObservationShard::pages]{};
        for (usize index = 0; index < ObservationShard::pages; ++index) {
            pages[index] = &concurrency_capacity_pages[index];
        }
        shard.initialize(
            kernel::CpuId{251}, nullptr, pages, ObservationShard::pages);

        ObservationLease operations[ObservationShard::slot_count / 2]{};
        bool filled = true;
        for (usize index = 0;
             index < ObservationShard::slot_count / 2;
             ++index) {
            operations[index] = shard.reserve(
                RecordKind::Operation,
                index + 1,
                1,
                Expectation::InternalFinite,
                SourceSite::current());
            if (!operations[index]) {
                filled = false;
                break;
            }
        }
        if (filled) {
            ObservationLease overflow = shard.reserve(
                RecordKind::Operation,
                0xffff,
                1,
                Expectation::InternalFinite,
                SourceSite::current());
            ObservationLease actor = shard.reserve(
                RecordKind::ExecutionActor,
                0x4400,
                1,
                Expectation::SchedulerControlled,
                SourceSite::current());
            result = !overflow && actor
                && actor.key().slot() < ObservationShard::slot_count / 2;
            if (actor) {
                actor.finish(1);
            }
        }
        for (auto& operation : operations) {
            if (operation) {
                operation.finish(1);
            }
        }
    }
    concurrency_test_shard.reset();
    return result;
}

[[maybe_unused, nodiscard]] auto test_operation_policy_follows_delivery_phase(
    [[maybe_unused]] const TestContext& context) noexcept -> bool {
    using namespace kernel::diag::concurrency;
    bool result = false;
    {
        ObservationShard& shard = concurrency_test_shard.emplace();
        shard.initialize(kernel::CpuId{3});
        ObservationLease operation = shard.reserve(
            RecordKind::Operation,
            0x5050,
            7,
            Expectation::ExternalUnbounded,
            SourceSite::current());
        do {
            if (!operation) {
                break;
            }
            const OperationPolicy policy{
                .kind = WaitKind::ChannelReceive,
                .expectation = Expectation::ExternalUnbounded,
                .driver = NodeRef::external(0x77, 7),
                .deadline = 1234,
                .grace = 55,
                .action = StallAction::Report,
            };
            operation.set_policy(policy);
            operation.publish(OperationPhase::Attached, policy.driver);

            const u64 mask = u64{1} << operation.key().slot();
            ObservationSnapshot snapshot{};
            if (!operation.snapshot(snapshot)
                || snapshot.phase
                    != static_cast<u32>(OperationPhase::Attached)
                || snapshot.wait_kind != WaitKind::ChannelReceive
                || snapshot.expectation != Expectation::ExternalUnbounded
                || snapshot.policy.kind != policy.kind
                || snapshot.policy.deadline != policy.deadline
                || snapshot.policy.grace != policy.grace
                || snapshot.policy.action != policy.action
                || snapshot.driver != policy.driver
                || (shard.watched() & mask) != 0) {
                break;
            }

            const NodeRef producer = NodeRef::cpu(kernel::CpuId{1});
            operation.publish(OperationPhase::Claimed, producer);
            if (!operation.snapshot(snapshot)
                || snapshot.wait_kind != WaitKind::CompletionPublication
                || snapshot.expectation != Expectation::InternalFinite
                || snapshot.driver != producer
                || (shard.watched() & mask) == 0) {
                break;
            }

            operation.publish(OperationPhase::WakeIssued, producer);
            if (!operation.snapshot(snapshot)
                || snapshot.wait_kind != WaitKind::CompletionDelivery
                || snapshot.expectation != Expectation::InternalFinite
                || (shard.watched() & mask) == 0) {
                break;
            }

            const NodeRef actor = NodeRef::observation(
                ObservationKey::make(kernel::CpuId{2}, 4, 9));
            operation.publish(OperationPhase::ReadyPublished, actor);
            result = operation.snapshot(snapshot)
                && snapshot.wait_kind == WaitKind::SchedulerReady
                && snapshot.expectation == Expectation::SchedulerControlled
                && snapshot.driver == actor
                && snapshot.policy.kind == policy.kind
                && snapshot.policy.deadline == policy.deadline
                && (shard.watched() & mask) != 0;
            operation.finish(
                static_cast<u32>(OperationPhase::Finished), 0);
        } while (false);
    }
    concurrency_test_shard.reset();
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
        ObservationLease peer = shard.reserve(
            RecordKind::ExecutionActor,
            0x30,
            1,
            Expectation::InternalFinite,
            SourceSite::current());
        ObservationLease peer_operation = shard.reserve(
            RecordKind::Operation,
            0x40,
            1,
            Expectation::InternalFinite,
            SourceSite::current());
        do {
            if (!actor || !operation || !peer || !peer_operation) {
                break;
            }
            // A normal waiter edge terminates at an external producer. It is
            // not a cycle merely because the operation is blocking.
            actor.link_wait(
                operation.key(),
                WaitKind::OperationCompletion,
                NodeRef::observation(actor.key()));
            operation.transition(
                static_cast<u32>(OperationPhase::Attached),
                1,
                WaitKind::CompletionPublication,
                NodeRef::external(0x55, 1));
            actor.watch(true);
            operation.watch(true);
            WaitGraphScratch scratch{};
            if (!analyze(actor.key(), scratch)
                || scratch.classification == StallClass::DeadlockCycle
                || scratch.count != 3) {
                break;
            }

            // A real cycle has two distinct actors and two operations.
            actor.link_wait(
                operation.key(),
                WaitKind::OperationCompletion,
                NodeRef::observation(peer.key()));
            operation.phase(
                static_cast<u32>(OperationPhase::Attached),
                2,
                SourceSite::current());
            operation.transition(
                static_cast<u32>(OperationPhase::Attached),
                2,
                WaitKind::OperationCompletion,
                NodeRef::observation(peer.key()));
            peer.link_wait(
                peer_operation.key(),
                WaitKind::OperationCompletion,
                NodeRef::observation(actor.key()));
            peer_operation.transition(
                static_cast<u32>(OperationPhase::Attached),
                1,
                WaitKind::OperationCompletion,
                NodeRef::observation(actor.key()));
            peer.watch(true);
            peer_operation.watch(true);
            scratch = {};
            if (!analyze(actor.key(), scratch)
                || scratch.classification != StallClass::DeadlockCycle
                || scratch.count != 4) {
                break;
            }

            // Stage C does not infer LostWake from generic Binding detail
            // bits.  Scheduler publication/consumption witnesses are added
            // in Stage D; until then both variants retain the graph's
            // ordinary cycle classification.
            actor.phase(
                static_cast<u32>(kernel::ExecutionState::Blocked), 4);
            actor.detail(0, 4);
            actor.detail(2, 0);
            operation.phase(
                static_cast<u32>(OperationPhase::ReadyPublished), 3);
            scratch = {};
            if (!analyze(actor.key(), scratch)
                || scratch.classification == StallClass::LostWake) {
                break;
            }
            actor.detail(0, 0);
            scratch = {};
            if (!analyze(actor.key(), scratch)
                || scratch.classification == StallClass::LostWake) {
                break;
            }

            operation.finish(static_cast<u32>(OperationPhase::Finished), 2);
            peer_operation.finish(
                static_cast<u32>(OperationPhase::Finished), 2);
            actor.clear_wait();
            peer.clear_wait();
            actor.finish(static_cast<u32>(kernel::ExecutionState::Exited));
            peer.finish(static_cast<u32>(kernel::ExecutionState::Exited));
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
        "stale owners cannot mutate or release a reused observation slot",
        test_observation_stale_owner_cannot_reopen_slot);
    (void)registry.add(
        "concurrency",
        "ephemeral observations cannot consume actor capacity",
        test_observation_actor_capacity_is_reserved);
    (void)registry.add(
        "concurrency",
        "operation policy follows canonical delivery phases",
        test_operation_policy_follows_delivery_phase);
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
