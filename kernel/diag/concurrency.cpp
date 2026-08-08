#include <diag/owner.hpp>
#include <diag/concurrency_private.hpp>
#include <diag/scenario.hpp>

#include <arch/cpu.hpp>
#include <arch/interrupt.hpp>
#include <arch/time.hpp>
#include <cpu/cpu_local.hpp>
#include <cpu/cpu_registry.hpp>
#include <cpu/cpu_runtime.hpp>
#include <diag/console.hpp>
#include <diag/panic.hpp>
#include <execution/execution.hpp>
#include <libk/manual_lifetime.hpp>
#include <libk/utility.hpp>
#include <mm/pmm.hpp>
#include <sched/dispatcher.hpp>
#include <sched/remote_queue.hpp>
#include <sync/irq_lock_guard.hpp>



namespace kernel::diag::concurrency {

static auto current_core() noexcept -> CpuDiagnosticsCore*;
static auto current_shard() noexcept -> ObservationShard*;

namespace {

constexpr u64 generation_mask = (u64{1} << 40) - 1;
constexpr u64 slot_generation_shift = 24;
constexpr u64 slot_pin_mask = (u64{1} << 22) - 1;
constexpr u64 slot_kind_shift = 22;
constexpr u64 slot_kind_mask = u64{3} << slot_kind_shift;
constexpr u64 slot_generation_mask =
    generation_mask << slot_generation_shift;

enum class SlotKind : u8 {
    Free,
    Reserved,
    Active,
    Retiring,
};

[[nodiscard]] constexpr auto make_slot_state(
    u64 generation,
    SlotKind kind,
    u64 pins = 0) noexcept -> u64 {
    return (generation << slot_generation_shift)
        | (static_cast<u64>(kind) << slot_kind_shift)
        | pins;
}

[[nodiscard]] constexpr auto slot_generation(u64 state) noexcept -> u64 {
    return (state & slot_generation_mask) >> slot_generation_shift;
}

[[nodiscard]] constexpr auto slot_kind(u64 state) noexcept -> SlotKind {
    return static_cast<SlotKind>(
        (state & slot_kind_mask) >> slot_kind_shift);
}

[[nodiscard]] constexpr auto slot_pins(u64 state) noexcept -> u64 {
    return state & slot_pin_mask;
}

[[nodiscard]] constexpr auto persistent(RecordKind kind) noexcept -> bool {
    return kind == RecordKind::ExecutionActor
        || kind == RecordKind::ServiceActor;
}

constexpr u64 wait_token_depth_mask = 0x7;
constexpr u64 wait_token_overflow = wait_token_depth_mask;
constexpr u64 wait_token_generation_max =
    libk::numeric_limits<u64>::max() >> 3;

[[nodiscard]] constexpr auto make_wait_token(
    u64 generation,
    u64 depth_code) noexcept -> WaitToken {
    return WaitToken{(generation << 3) | depth_code};
}

[[nodiscard]] constexpr auto wait_token_depth(WaitToken token) noexcept
    -> u64 {
    return token.raw & wait_token_depth_mask;
}








#if MYOS_CONCURRENCY_DIAG >= 3
ReportNotifier report_notifier{};
#endif

[[nodiscard]] auto now() noexcept -> u64 {
    return arch::read_clock().ticks();
}

[[nodiscard]] auto elapsed(u64 now_tick, u64 then) noexcept -> u64 {
    return now_tick >= then ? now_tick - then : 0;
}

[[nodiscard]] constexpr auto bit(usize index) noexcept -> u64 {
    return u64{1} << index;
}

[[nodiscard]] constexpr auto node_kind(NodeRef node) noexcept -> u32 {
    return static_cast<u32>(node.kind);
}

[[nodiscard]] auto add_sat(u64 left, u64 right) noexcept -> u64 {
    const u64 limit = libk::numeric_limits<u64>::max();
    return right > limit - left ? limit : left + right;
}

#if MYOS_CONCURRENCY_DIAG >= 1
enum class HashMode : u8 {
    Coherent,
    Relation,
};

[[nodiscard]] constexpr auto mix_hash(u64 hash, u64 value) noexcept -> u64 {
    hash ^= value + u64{0x9e3779b97f4a7c15}
        + (hash << 6) + (hash >> 2);
    return hash;
}

[[nodiscard]] constexpr auto cpu_stall_generation(
    u64 entered,
    u32 phase) noexcept -> u64 {
    constexpr u64 mask = (u64{1} << 56) - 1;
    const u64 generation =
        mix_hash(mix_hash(u64{0x7e92d8a153}, entered), phase) & mask;
    return generation <= NodeRef::cpu_generation
        ? generation + 2 : generation;
}

[[nodiscard]] auto snapshot_hash(
    const ObservationSnapshot& snapshot,
    HashMode mode = HashMode::Coherent) noexcept -> u64 {
    u64 hash = u64{0x243f6a8885a308d3};
    hash = mix_hash(hash, snapshot.generation);
    hash = mix_hash(hash, static_cast<u64>(snapshot.evidence));
    // Coherent graph fingerprints include both counters so a second snapshot
    // notices any concurrent update. Stall candidates compare semantic state;
    // their activity interval is checked below and progress is compared
    // separately.
    if (mode == HashMode::Coherent) {
        hash = mix_hash(hash, snapshot.activity_epoch);
        hash = mix_hash(hash, snapshot.progress_epoch);
    }
    hash = mix_hash(hash, static_cast<u64>(snapshot.record_kind));
    hash = mix_hash(hash, snapshot.phase);
    hash = mix_hash(hash, static_cast<u64>(snapshot.wait_kind));
    hash = mix_hash(hash, static_cast<u64>(snapshot.expectation));
    hash = mix_hash(hash, static_cast<u64>(snapshot.policy.kind));
    hash = mix_hash(hash, static_cast<u64>(snapshot.policy.expectation));
    hash = mix_hash(hash, snapshot.policy.driver.identity);
    hash = mix_hash(hash, static_cast<u64>(snapshot.policy.driver.kind));
    hash = mix_hash(hash, snapshot.policy.driver.generation);
    hash = mix_hash(hash, snapshot.policy.deadline_driver.identity);
    hash = mix_hash(
        hash, static_cast<u64>(snapshot.policy.deadline_driver.kind));
    hash = mix_hash(hash, snapshot.policy.deadline_driver.generation);
    hash = mix_hash(hash, snapshot.policy.deadline);
    hash = mix_hash(hash, snapshot.policy.grace);
    hash = mix_hash(hash, static_cast<u64>(snapshot.policy.action));
    hash = mix_hash(hash, snapshot.subject_identity);
    hash = mix_hash(hash, snapshot.subject_generation);
    hash = mix_hash(hash, snapshot.wait_target.raw);
    hash = mix_hash(hash, snapshot.driver.identity);
    hash = mix_hash(hash, static_cast<u64>(snapshot.driver.kind));
    hash = mix_hash(hash, snapshot.driver.generation);
    hash = mix_hash(hash, snapshot.blocker.identity);
    hash = mix_hash(hash, static_cast<u64>(snapshot.blocker.kind));
    hash = mix_hash(hash, snapshot.blocker.generation);
    if (mode == HashMode::Coherent) {
        hash = mix_hash(hash, snapshot.semantic_stamp);
        for (const u64 value : snapshot.detail) {
            hash = mix_hash(hash, value);
        }
    }
    return hash;
}
#endif

void increment_sat(libk::Atomic<u64>& value, u64 delta) noexcept {
    u64 current = value.load<libk::MemoryOrder::Relaxed>();
    for (;;) {
        const u64 next = add_sat(current, delta);
        if (value.compare_exchange_weak<
                libk::MemoryOrder::Relaxed,
                libk::MemoryOrder::Relaxed>(current, next)) {
            return;
        }
    }
}

#if MYOS_CONCURRENCY_DIAG >= 3
struct WatchdogThresholds final {
    u64 anchor{};
    u64 soft_at{};
    u64 hard_at{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return soft_at != 0 && hard_at >= soft_at;
    }
};

[[nodiscard]] auto queue_report(
    CpuId watcher,
    CpuId target,
    NodeRef root,
    WatchdogCandidate::State state,
    StallClass classification,
    EvidenceGrade evidence,
    u64 age,
    const ObservationSnapshot* root_snapshot,
    const WaitGraphScratch& graph) noexcept -> bool {
    CpuDiagnosticsCore* const core = current_core();
    if (core == nullptr) {
        return false;
    }
    ReportRecord report{
        .watcher = watcher,
        .target = target,
        .root = root,
        .state = state,
        .classification = classification,
        .evidence = evidence,
        .age = age,
        .root_snapshot = root_snapshot != nullptr ? *root_snapshot
                                                    : ObservationSnapshot{},
        .root_snapshot_valid = root_snapshot != nullptr,
        .graph = graph,
    };
    const bool queued = core->reports.publish(report);
    if (!queued) {
        static_cast<void>(core->status().flags.fetch_or<
            libk::MemoryOrder::Release>(DiagnosticStatus::ReportDropped));
        return false;
    }
    // A wake is only a scheduling hint. The queue owns the report payload and
    // remains correct if no consumer has registered yet.  notify() is a
    // bounded publication/read attempt; it never waits for the scheduler or
    // notifier teardown.
    if (report_notifier.notify()) {
    }
    return true;
}

[[nodiscard]] auto thresholds_for(
    const WatchdogPolicy& policy,
    const ObservationSnapshot& snapshot,
    const ObservationSnapshot* wait_target = nullptr) noexcept
    -> WatchdogThresholds {
    if (snapshot.record_kind == RecordKind::ExecutionActor) {
        const auto state = static_cast<ExecutionState>(snapshot.phase);
        switch (state) {
        case ExecutionState::Ready:
            return {
                snapshot.last_progress_at,
                add_sat(snapshot.last_progress_at, policy.scheduler_soft),
                add_sat(snapshot.last_progress_at, policy.scheduler_hard),
            };
        case ExecutionState::Throttled: {
            const u64 deadline = snapshot.policy.deadline;
            if (deadline == 0) {
                return {};
            }
            const u64 grace = snapshot.policy.grace != 0
                ? snapshot.policy.grace : policy.scheduler_hard;
            return {deadline, deadline, add_sat(deadline, grace)};
        }
        case ExecutionState::Blocked:
            return wait_target == nullptr
                ? WatchdogThresholds{}
                : thresholds_for(policy, *wait_target);
        case ExecutionState::Parked:
            if (snapshot.wait_kind != WaitKind::SchedulerActivation) {
                return {};
            }
            return {
                snapshot.last_progress_at,
                add_sat(snapshot.last_progress_at, policy.scheduler_soft),
                add_sat(snapshot.last_progress_at, policy.scheduler_hard),
            };
        case ExecutionState::Prepared:
        case ExecutionState::Running:
        case ExecutionState::Exited:
            return {};
        }
    }

    if (snapshot.expectation == Expectation::ExternalUnbounded
        || snapshot.expectation == Expectation::Idle
        || snapshot.expectation == Expectation::ObserveOnly) {
        return {};
    }

    if (snapshot.expectation == Expectation::DeadlineBound) {
        const u64 deadline = snapshot.policy.deadline;
        if (deadline == 0) {
            return {};
        }
        const u64 grace = snapshot.policy.grace != 0
            ? snapshot.policy.grace : policy.critical_hard;
        return {deadline, deadline, add_sat(deadline, grace)};
    }

    if (snapshot.wait_kind == WaitKind::None
        || snapshot.wait_kind == WaitKind::OperationCompletion) {
        return {};
    }

    u64 soft{};
    u64 hard{};
    switch (snapshot.wait_kind) {
    case WaitKind::SpinLock:
    case WaitKind::CompletionPublication:
        soft = policy.critical_soft;
        hard = policy.critical_hard;
        break;
    case WaitKind::CompletionDelivery:
    case WaitKind::RemoteRequest:
    case WaitKind::IpiDelivery:
    case WaitKind::ShootdownAck:
        soft = policy.transport_soft;
        hard = policy.transport_hard;
        break;
    case WaitKind::SchedulerReady:
    case WaitKind::SchedulerRefill:
    case WaitKind::SchedulerWake:
    case WaitKind::SchedulerActivation:
        soft = policy.scheduler_soft;
        hard = policy.scheduler_hard;
        break;
    default:
        break;
    }
    if (soft == 0 && snapshot.expectation == Expectation::SchedulerControlled) {
        soft = policy.scheduler_soft;
        hard = policy.scheduler_hard;
    } else if (soft == 0) {
        soft = policy.service_soft;
        hard = policy.service_hard;
    }
    return {
        snapshot.last_progress_at,
        add_sat(snapshot.last_progress_at, soft),
        add_sat(snapshot.last_progress_at, hard),
    };
}

#endif

#if MYOS_CONCURRENCY_DIAG >= 4
[[nodiscard]] constexpr auto profile_bucket(u64 duration) noexcept -> usize {
    usize bucket{};
    while (duration > 1 && bucket + 1 < profile_bucket_count) {
        duration >>= 1;
        ++bucket;
    }
    return bucket;
}
#endif

void clear_record(ObservationRecord& record) noexcept {
    record.sequence.store<libk::MemoryOrder::Relaxed>(0);
    record.activity_epoch.store<libk::MemoryOrder::Relaxed>(0);
    record.progress_epoch.store<libk::MemoryOrder::Relaxed>(0);
    record.started_at.store<libk::MemoryOrder::Relaxed>(0);
    record.last_activity_at.store<libk::MemoryOrder::Relaxed>(0);
    record.last_progress_at.store<libk::MemoryOrder::Relaxed>(0);
    record.evidence.store<libk::MemoryOrder::Relaxed>(
        static_cast<u32>(EvidenceGrade::None));
    record.record_kind.store<libk::MemoryOrder::Relaxed>(0);
    record.phase.store<libk::MemoryOrder::Relaxed>(0);
    record.wait_kind.store<libk::MemoryOrder::Relaxed>(0);
    record.expectation.store<libk::MemoryOrder::Relaxed>(0);
    record.policy_kinds.store<libk::MemoryOrder::Relaxed>(0);
    record.policy_driver_key.store<libk::MemoryOrder::Relaxed>(0);
    record.policy_driver_generation.store<libk::MemoryOrder::Relaxed>(0);
    record.subject_identity.store<libk::MemoryOrder::Relaxed>(0);
    record.subject_generation.store<libk::MemoryOrder::Relaxed>(0);
    record.wait_target.store<libk::MemoryOrder::Relaxed>(0);
    record.driver_key.store<libk::MemoryOrder::Relaxed>(0);
    record.driver_kind.store<libk::MemoryOrder::Relaxed>(0);
    record.driver_generation.store<libk::MemoryOrder::Relaxed>(0);
    record.blocker_key.store<libk::MemoryOrder::Relaxed>(0);
    record.blocker_kind.store<libk::MemoryOrder::Relaxed>(0);
    record.blocker_generation.store<libk::MemoryOrder::Relaxed>(0);
    record.semantic_stamp.store<libk::MemoryOrder::Relaxed>(0);
    record.deadline.store<libk::MemoryOrder::Relaxed>(0);
    record.grace.store<libk::MemoryOrder::Relaxed>(0);
    record.site_file.store<libk::MemoryOrder::Relaxed>(0);
    record.site_function.store<libk::MemoryOrder::Relaxed>(0);
    record.site_line.store<libk::MemoryOrder::Relaxed>(0);
    for (auto& detail : record.detail) {
        detail.store<libk::MemoryOrder::Relaxed>(0);
    }
}

void clear_page(ObservationPage& page) noexcept {
    for (usize index = 0; index < ObservationPage::slot_count; ++index) {
        page.records[index].slot_state.store<libk::MemoryOrder::Relaxed>(
            make_slot_state(0, SlotKind::Free));
        clear_record(page.records[index]);
    }
}

void publish_node(
    libk::Atomic<u64>& identity,
    libk::Atomic<u32>& kind,
    libk::Atomic<u64>& generation,
    NodeRef value) noexcept {
    identity.store<libk::MemoryOrder::Relaxed>(value.identity);
    kind.store<libk::MemoryOrder::Relaxed>(node_kind(value));
    generation.store<libk::MemoryOrder::Relaxed>(value.generation);
}

[[nodiscard]] auto read_node(
    u64 identity,
    u32 kind,
    u64 generation) noexcept -> NodeRef {
    return NodeRef{
        static_cast<NodeRef::Kind>(kind), identity, generation};
}

void degrade_record(ObservationRecord& record) noexcept {
    record.evidence.store<libk::MemoryOrder::Release>(
        static_cast<u32>(EvidenceGrade::Degraded));
}

} // namespace

auto provision(
    CpuRuntime& runtime,
    CpuId id,
    mm::Pmm& pmm,
    const WatchdogPolicy& policy) noexcept -> bool {
    if (runtime.diagnostics == nullptr) {
        auto owner_page = pmm.allocate_page();
        if (!owner_page) {
            return false;
        }
        runtime.diagnostics_page = libk::move(owner_page).value();
        runtime.diagnostics = libk::construct_at(
            reinterpret_cast<CpuDiagnostics*>(
                runtime.diagnostics_page.bytes()));
    }
#if MYOS_CONCURRENCY_DIAG == 0
    static_cast<void>(id);
    static_cast<void>(policy);
    return true;
#else
    auto* const owner = runtime.diagnostics;
    auto core_page = pmm.allocate_page();
    if (!core_page) {
        return true;
    }
    owner->concurrency_page = libk::move(core_page).value();
    owner->concurrency = libk::construct_at(
        reinterpret_cast<CpuDiagnosticsCore*>(
            owner->concurrency_page.bytes()));
    auto* const core = owner->concurrency;
    core->policy = policy;
    core->live.current_actor.store<libk::MemoryOrder::Relaxed>(0);
    core->status().flags.store<libk::MemoryOrder::Relaxed>(0);

    if (auto store_page = pmm.allocate_page(); store_page) {
        owner->observation_store_page = libk::move(store_page).value();
        auto* const store = libk::construct_at(
            reinterpret_cast<ObservationStore*>(
                owner->observation_store_page.bytes()));
        ObservationPage* storage[ObservationShard::pages]{};
        usize count{};
        for (; count < ObservationShard::pages; ++count) {
            auto page = pmm.allocate_page();
            if (!page) {
                break;
            }
            owner->observation_pages[count] = libk::move(page).value();
            storage[count] = libk::construct_at(
                reinterpret_cast<ObservationPage*>(
                    owner->observation_pages[count].bytes()));
        }
        store->shard.initialize(
            id, &store->profile, storage, count, &store->status);
        core->status_store = &store->status;
        core->profile = &store->profile;
        core->observations = &store->shard;
        if (count != ObservationShard::pages) {
            static_cast<void>(core->status().flags.fetch_or<
                libk::MemoryOrder::Release>(
                    DiagnosticStatus::ObservationCapacity));
        }
    } else {
        static_cast<void>(core->status().flags.fetch_or<
            libk::MemoryOrder::Release>(DiagnosticStatus::StorageMissing));
    }

    if (enabled(Level::Trace)) {
        if (auto flight_page = pmm.allocate_page(); flight_page) {
            owner->flight_page = libk::move(flight_page).value();
            FlightPage* storage[FlightRecorder::page_count]{};
            usize count{};
            for (; count < FlightRecorder::page_count; ++count) {
                auto page = pmm.allocate_page();
                if (!page) {
                    break;
                }
                owner->flight_records[count] = libk::move(page).value();
                storage[count] = libk::construct_at(
                    reinterpret_cast<FlightPage*>(
                        owner->flight_records[count].bytes()));
            }
            if (count == FlightRecorder::page_count) {
                auto* const flight = libk::construct_at(
                    reinterpret_cast<FlightRecorder*>(
                        owner->flight_page.bytes()));
                flight->initialize(id, storage);
                core->flight = flight;
            } else {
                static_cast<void>(core->status().flags.fetch_or<
                    libk::MemoryOrder::Release>(
                        DiagnosticStatus::StorageMissing));
            }
        } else {
            static_cast<void>(core->status().flags.fetch_or<
                libk::MemoryOrder::Release>(
                    DiagnosticStatus::StorageMissing));
        }
    }
    return true;
#endif
}

auto panic_slot(CpuRuntime& runtime) noexcept -> diag::PanicSlot* {
    return runtime.diagnostics == nullptr ? nullptr : &runtime.diagnostics->panic;
}

void destroy(CpuRuntime& runtime) noexcept {
    CpuDiagnostics* const owner = runtime.diagnostics;
    if (owner == nullptr) {
        return;
    }
    CpuDiagnosticsCore* const core = owner->concurrency;
    if (core != nullptr) {
        if (core->flight != nullptr) {
            libk::destroy_at(core->flight);
            core->flight = nullptr;
        }
        libk::destroy_at(core);
        owner->concurrency = nullptr;
    }
    if (owner->observation_store_page) {
        libk::destroy_at(reinterpret_cast<ObservationStore*>(
            owner->observation_store_page.bytes()));
        owner->observation_store_page.reset();
    }
    for (auto& page : owner->observation_pages) {
        if (page) {
            libk::destroy_at(reinterpret_cast<ObservationPage*>(page.bytes()));
            page.reset();
        }
    }
    for (auto& page : owner->flight_records) {
        if (page) {
            libk::destroy_at(reinterpret_cast<FlightPage*>(page.bytes()));
            page.reset();
        }
    }
    owner->flight_page.reset();
    owner->concurrency_page.reset();
    if (owner->lock_profile_page) {
        libk::destroy_at(reinterpret_cast<sync::LockProfile*>(
            owner->lock_profile_page.bytes()));
        owner->lock_profile_page.reset();
    }
    libk::destroy_at(owner);
    runtime.diagnostics = nullptr;
    runtime.diagnostics_page.reset();
}

[[nodiscard]] auto owner_core(const CpuRuntime& runtime) noexcept
    -> const CpuDiagnosticsCore* {
    return runtime.diagnostics == nullptr
        ? nullptr : runtime.diagnostics->concurrency;
}

void dump_flight(CpuId id, const CpuRuntime& runtime) noexcept {
    if (!enabled(Level::Trace)) {
        return;
    }
    const auto* const owner = runtime.diagnostics;
    const auto* const core = owner == nullptr ? nullptr : owner->concurrency;
    const auto* const flight = core == nullptr ? nullptr : core->flight;
    if (flight == nullptr) {
        return;
    }
    dump_flight(id, *flight);
}

[[nodiscard]] auto flight_head(const CpuRuntime& runtime) noexcept -> u64 {
    const CpuDiagnosticsCore* const core = owner_core(runtime);
    return core == nullptr || core->flight == nullptr
        ? 0 : core->flight->head();
}

[[nodiscard]] auto flight_count(const CpuRuntime& runtime) noexcept -> usize {
    const CpuDiagnosticsCore* const core = owner_core(runtime);
    if (core == nullptr || core->flight == nullptr) {
        return 0;
    }
    const u64 head = core->flight->head();
    return head < FlightRecorder::capacity
        ? static_cast<usize>(head) : FlightRecorder::capacity;
}

[[nodiscard]] auto flight_read(
    const CpuRuntime& runtime,
    usize index,
    FlightRecordValue& result) noexcept -> bool {
    const CpuDiagnosticsCore* const core = owner_core(runtime);
    return core != nullptr && core->flight != nullptr
        && core->flight->read(index, result);
}

[[nodiscard]] auto observation_snapshot(
    const CpuRuntime& runtime,
    ObservationKey key,
    ObservationSnapshot& result) noexcept -> bool {
    const CpuDiagnosticsCore* const core = owner_core(runtime);
    return core != nullptr && core->observations != nullptr
        && core->observations->snapshot(key, result);
}

[[nodiscard]] auto observation_key_at(
    const CpuRuntime& runtime,
    usize index) noexcept -> ObservationKey {
    const CpuDiagnosticsCore* const core = owner_core(runtime);
    return core == nullptr || core->observations == nullptr
        ? ObservationKey{} : core->observations->key_at(index);
}

[[nodiscard]] auto observation_slot_count() noexcept -> usize {
    return ObservationShard::slot_count;
}

[[nodiscard]] auto observation_watched(const CpuRuntime& runtime) noexcept
    -> u64 {
    const CpuDiagnosticsCore* const core = owner_core(runtime);
    return core == nullptr || core->observations == nullptr
        ? 0 : core->observations->watched();
}

[[nodiscard]] auto status_flags(const CpuRuntime& runtime) noexcept -> u32 {
    const CpuDiagnosticsCore* const core = owner_core(runtime);
    return core == nullptr
        ? 0 : core->status().flags.load<libk::MemoryOrder::Acquire>();
}

[[nodiscard]] auto report_pending(const CpuRuntime& runtime) noexcept -> bool {
    const CpuDiagnosticsCore* const core = owner_core(runtime);
    return core != nullptr && core->reports.pending();
}


auto AtomicSnapshotWriter::begin(
    libk::Atomic<u64>& sequence,
    u64& odd) noexcept -> bool {
    u64 current = sequence.load<libk::MemoryOrder::Acquire>();
    if ((current & 1U) != 0
        || current >= libk::numeric_limits<u64>::max() - 2) {
        odd = 0;
        return false;
    }
    odd = current + 1;
    return sequence.compare_exchange_strong<
        libk::MemoryOrder::AcqRel,
        libk::MemoryOrder::Acquire>(current, odd);
}

void AtomicSnapshotWriter::end(
    libk::Atomic<u64>& sequence,
    u64 odd) noexcept {
    if (odd != 0) {
        sequence.store<libk::MemoryOrder::Release>(odd + 1);
    }
}

auto AtomicSnapshotReader::begin(
    const libk::Atomic<u64>& sequence) noexcept -> u64 {
    return sequence.load<libk::MemoryOrder::Acquire>();
}

auto AtomicSnapshotReader::valid(
    const libk::Atomic<u64>& sequence,
    u64 first) noexcept -> bool {
    if ((first & 1U) != 0) {
        return false;
    }
    const u64 last = sequence.load<libk::MemoryOrder::Acquire>();
    return first == last && (last & 1U) == 0;
}

void ObservationShard::initialize(
    CpuId id,
    LatencyProfile* profile,
    ObservationPage* const* storage,
    usize page_count,
    DiagnosticStatus* status) noexcept {
    id_ = id;
    profile_ = profile;
    status_ = status;
    if (profile_ != nullptr) {
        profile_->initialize();
    }
    page_count_ = 0;
    for (auto& page : storage_) {
        page = nullptr;
    }
    if (storage != nullptr) {
        page_count = page_count > pages ? pages : page_count;
        for (usize index = 0; index < page_count; ++index) {
            if (storage[index] == nullptr) {
                break;
            }
            storage_[index] = storage[index];
            clear_page(*storage_[index]);
            ++page_count_;
        }
    }
    allocated_.store<libk::MemoryOrder::Relaxed>(0);
    watched_.store<libk::MemoryOrder::Relaxed>(0);
    degraded_.store<libk::MemoryOrder::Relaxed>(0);
}

void LatencyProfile::initialize() noexcept {
    for (auto& stats : records) {
        stats.completed.store<libk::MemoryOrder::Relaxed>(0);
        stats.total.store<libk::MemoryOrder::Relaxed>(0);
        stats.max.store<libk::MemoryOrder::Relaxed>(0);
        stats.current.store<libk::MemoryOrder::Relaxed>(0);
        for (auto& bucket : stats.buckets) {
            bucket.store<libk::MemoryOrder::Relaxed>(0);
        }
    }
}

void ObservationShard::profile_finish(
    RecordKind kind,
    u64 duration) noexcept {
#if MYOS_CONCURRENCY_DIAG >= 4
    if (profile_ == nullptr || kind >= RecordKind::Count) {
        return;
    }
    LatencyStats& stats = profile_->records[static_cast<usize>(kind)];
    increment_sat(stats.completed, 1);
    increment_sat(stats.total, duration);
    u64 current = stats.max.load<libk::MemoryOrder::Relaxed>();
    while (current < duration
        && !stats.max.compare_exchange_weak<
            libk::MemoryOrder::Relaxed,
            libk::MemoryOrder::Relaxed>(current, duration)) {
    }
    increment_sat(stats.buckets[profile_bucket(duration)], 1);
#else
    static_cast<void>(kind);
    static_cast<void>(duration);
#endif
}

void ObservationShard::profile_current(u64 tick) const noexcept {
#if MYOS_CONCURRENCY_DIAG >= 4
    if (profile_ == nullptr) {
        return;
    }
    for (auto& stats : profile_->records) {
        stats.current.store<libk::MemoryOrder::Relaxed>(0);
    }
    const u64 allocated = allocated_.load<libk::MemoryOrder::Acquire>();
    for (usize index = 0; index < slot_count; ++index) {
        if ((allocated & bit(index)) == 0) {
            continue;
        }
        usize local{};
        ObservationPage* const page = page_for(index, local);
        if (page == nullptr) {
            continue;
        }
        ObservationSnapshot snapshot{};
        const u64 state = page->records[local].slot_state.load<
            libk::MemoryOrder::Acquire>();
        if (slot_kind(state) != SlotKind::Active) {
            continue;
        }
        const u64 generation = slot_generation(state);
        const ObservationKey key = ObservationKey::make(
            id_, static_cast<u16>(index), generation);
        if (!key || !this->snapshot(key, snapshot)) {
            continue;
        }
        const usize kind = static_cast<usize>(snapshot.record_kind);
        if (kind >= static_cast<usize>(RecordKind::Count)) {
            continue;
        }
        const u64 duration = snapshot.started_at != 0
            ? elapsed(tick, snapshot.started_at) : 0;
        LatencyStats& stats = profile_->records[kind];
        u64 current = stats.current.load<libk::MemoryOrder::Relaxed>();
        while (current < duration
            && !stats.current.compare_exchange_weak<
                libk::MemoryOrder::Relaxed,
                libk::MemoryOrder::Relaxed>(current, duration)) {
        }
    }
#else
    static_cast<void>(tick);
#endif
}

ObservationShard::~ObservationShard() noexcept {
}

auto ObservationShard::page_for(
    usize index,
    usize& local) const noexcept -> ObservationPage* {
    if (index >= slot_count) {
        return nullptr;
    }
    const usize page_index = index / slots_per_page;
    local = index % slots_per_page;
    return page_index < page_count_ ? storage_[page_index] : nullptr;
}

auto ObservationShard::valid(ObservationKey key) const noexcept
    -> ObservationRecord* {
    const usize index = key.slot();
    if (!key || key.shard() != id_ || index >= slot_count) {
        return nullptr;
    }
    usize local{};
    ObservationPage* const page = page_for(index, local);
    if (page == nullptr) {
        return nullptr;
    }
    ObservationRecord& record = page->records[local];
    const u64 state = record.slot_state.load<libk::MemoryOrder::Acquire>();
    if (slot_kind(state) != SlotKind::Active
        || slot_generation(state) != key.generation()) {
        return nullptr;
    }
    return &record;
}

auto ObservationShard::active(
    ObservationKey key,
    const ObservationRecord& record) const noexcept -> bool {
    const u64 state = record.slot_state.load<libk::MemoryOrder::Acquire>();
    return slot_kind(state) == SlotKind::Active
        && slot_generation(state) == key.generation();
}

auto ObservationShard::pin(
    ObservationKey key,
    ObservationRecord*& result) const noexcept -> bool {
    result = nullptr;
    ObservationRecord* const record = valid(key);
    if (record == nullptr) {
        return false;
    }
    u64 state = record->slot_state.load<libk::MemoryOrder::Acquire>();
    for (;;) {
        if (slot_generation(state) != key.generation()
            || slot_kind(state) != SlotKind::Active
            || slot_pins(state) == slot_pin_mask) {
            return false;
        }
        if (record->slot_state.compare_exchange_weak<
                libk::MemoryOrder::AcqRel,
                libk::MemoryOrder::Acquire>(state, state + 1)) {
            result = record;
            return true;
        }
    }
}

void ObservationShard::unpin(
    ObservationKey key,
    ObservationRecord& record) const noexcept {
    const u64 state = record.slot_state.fetch_sub<
        libk::MemoryOrder::AcqRel>(1);
    if (slot_pins(state) == 0
        || slot_generation(state) != key.generation()
        || (slot_kind(state) != SlotKind::Active
            && slot_kind(state) != SlotKind::Retiring)) {
        const_cast<ObservationShard*>(this)->mark_degraded(
            DiagnosticStatus::ObservationLeaseCorrupt);
        return;
    }
    if (slot_pins(state) != 1 || slot_kind(state) != SlotKind::Retiring) {
        return;
    }
    const_cast<ObservationShard*>(this)->reclaim(key, record);
}

auto ObservationShard::retire(
    ObservationKey key,
    ObservationRecord& record) noexcept -> bool {
    u64 state = record.slot_state.load<libk::MemoryOrder::Acquire>();
    for (;;) {
        if (slot_generation(state) != key.generation()
            || slot_kind(state) != SlotKind::Active) {
            return false;
        }
        const u64 next = make_slot_state(
            key.generation(), SlotKind::Retiring, slot_pins(state));
        if (record.slot_state.compare_exchange_weak<
                libk::MemoryOrder::AcqRel,
                libk::MemoryOrder::Acquire>(state, next)) {
            return true;
        }
    }
}

void ObservationShard::reclaim(
    ObservationKey key,
    ObservationRecord& record) noexcept {
    const usize index = key.slot();
    if (!key || key.shard() != id_ || index >= slot_count) {
        mark_degraded(DiagnosticStatus::ObservationLeaseCorrupt);
        return;
    }

    // Directory masks are withdrawn before Free is published. A new
    // generation therefore cannot be claimed until every old-generation
    // summary write has completed.
    static_cast<void>(watched_.fetch_and<libk::MemoryOrder::Release>(
        ~bit(index)));
    static_cast<void>(allocated_.fetch_and<libk::MemoryOrder::Release>(
        ~bit(index)));

    u64 expected = make_slot_state(
        key.generation(), SlotKind::Retiring);
    if (!record.slot_state.compare_exchange_strong<
            libk::MemoryOrder::AcqRel,
            libk::MemoryOrder::Acquire>(
                expected,
                make_slot_state(key.generation(), SlotKind::Free))) {
        mark_degraded(DiagnosticStatus::ObservationLeaseCorrupt);
    }
}

auto ObservationShard::reserve(
    RecordKind kind,
    u64 subject_identity,
    u64 subject_generation,
    Expectation expectation,
    SourceSite site) noexcept -> ObservationLease {
    const usize available = static_cast<usize>(page_count_) * slots_per_page;
    const usize dedicated = page_count_ >= 2
        ? (static_cast<usize>(page_count_) / 2) * slots_per_page : 0;
    // Persistent actors prefer the dedicated half and may continue into
    // otherwise-unused ephemeral capacity. Ephemeral churn starts after the
    // dedicated range and can never consume it.
    const usize first = persistent(kind) || dedicated == 0 ? 0 : dedicated;

    const auto claim = [&](usize begin, usize end) noexcept
        -> ObservationLease {
        for (usize index = begin; index < end; ++index) {
            usize local{};
            ObservationPage* const page = page_for(index, local);
            if (page == nullptr) {
                continue;
            }
            ObservationRecord& record = page->records[local];
            u64 current = record.slot_state.load<libk::MemoryOrder::Acquire>();
            if (slot_kind(current) != SlotKind::Free) {
                continue;
            }
            const u64 generation = slot_generation(current);
            if (generation >= generation_mask) {
                mark_degraded(
                    DiagnosticStatus::ObservationGenerationExhausted);
                continue;
            }
            const u64 next_generation = generation + 1;
            if (!record.slot_state.compare_exchange_weak<
                    libk::MemoryOrder::AcqRel,
                    libk::MemoryOrder::Acquire>(
                        current,
                        make_slot_state(
                            next_generation, SlotKind::Reserved))) {
                if (index != begin) {
                    --index;
                }
                continue;
            }

            clear_record(record);
            record.record_kind.store<libk::MemoryOrder::Relaxed>(
                static_cast<u32>(kind));
            record.expectation.store<libk::MemoryOrder::Relaxed>(
                static_cast<u32>(expectation));
            record.policy_kinds.store<libk::MemoryOrder::Relaxed>(
                static_cast<u32>(StallAction::Report) << 16);
            record.subject_identity.store<libk::MemoryOrder::Relaxed>(
                subject_identity);
            record.subject_generation.store<libk::MemoryOrder::Relaxed>(
                subject_generation);
            record.site_file.store<libk::MemoryOrder::Relaxed>(
                reinterpret_cast<usize>(site.file));
            record.site_function.store<libk::MemoryOrder::Relaxed>(
                reinterpret_cast<usize>(site.function));
            record.site_line.store<libk::MemoryOrder::Relaxed>(site.line);
            const u64 tick = now();
            record.started_at.store<libk::MemoryOrder::Relaxed>(tick);
            record.last_activity_at.store<libk::MemoryOrder::Relaxed>(tick);
            record.last_progress_at.store<libk::MemoryOrder::Relaxed>(tick);
            record.slot_state.store<libk::MemoryOrder::Release>(
                make_slot_state(next_generation, SlotKind::Active));
            static_cast<void>(allocated_.fetch_or<
                libk::MemoryOrder::Release>(bit(index)));
            return ObservationLease{ObservationKey::make(
                id_, static_cast<u16>(index), next_generation)};
        }
        return {};
    };

    if (ObservationLease result = claim(first, available); result) {
        return result;
    }
    degraded_.store<libk::MemoryOrder::Release>(1);
    mark_degraded(DiagnosticStatus::ObservationCapacity);
    return {};
}

void ObservationShard::mark_degraded(u32 flag) noexcept {
    degraded_.store<libk::MemoryOrder::Release>(1);
    if (status_ != nullptr) {
        static_cast<void>(status_->flags.fetch_or<libk::MemoryOrder::Release>(
            flag));
        return;
    }
    if (CpuDiagnosticsCore* const core = current_core(); core != nullptr) {
        static_cast<void>(core->status().flags.fetch_or<libk::MemoryOrder::Release>(
            flag));
    }
}

auto ObservationShard::key_at(usize index) const noexcept -> ObservationKey {
    if (index >= slot_count) {
        return {};
    }
    usize local{};
    ObservationPage* const page = page_for(index, local);
    if (page == nullptr) {
        return {};
    }
    const u64 mask = bit(index);
    if ((allocated_.load<libk::MemoryOrder::Acquire>() & mask) == 0) {
        return {};
    }
    const ObservationRecord& record = page->records[local];
    const u64 state = record.slot_state.load<libk::MemoryOrder::Acquire>();
    if (slot_kind(state) != SlotKind::Active) {
        return {};
    }
    const u64 generation = slot_generation(state);
    return ObservationKey::make(id_, static_cast<u16>(index), generation);
}

void ObservationShard::release(ObservationKey key) noexcept {
    const usize index = key.slot();
    if (!key || key.shard() != id_ || index >= slot_count) {
        return;
    }
    usize local{};
    ObservationPage* const page = page_for(index, local);
    if (page == nullptr) {
        return;
    }
    ObservationRecord& record = page->records[local];
    if (!retire(key, record)) {
        return;
    }
    const u64 state = record.slot_state.load<libk::MemoryOrder::Acquire>();
    if (slot_kind(state) == SlotKind::Retiring
        && slot_generation(state) == key.generation()
        && slot_pins(state) == 0) {
        reclaim(key, record);
    }
}

auto ObservationShard::write_metadata(
    ObservationKey key,
    u32 phase,
    WaitKind wait,
    NodeRef driver,
    NodeRef blocker,
    u64 semantic_stamp,
    bool update_progress,
    SourceSite site) noexcept -> bool {
    ObservationRecord* record{};
    if (!pin(key, record)) {
        return false;
    }
    u64 odd{};
    if (!AtomicSnapshotWriter::begin(record->sequence, odd)) {
        degrade_record(*record);
        mark_degraded(DiagnosticStatus::ObservationWriterCollision);
        degraded_.store<libk::MemoryOrder::Release>(1);
        unpin(key, *record);
        return false;
    }
    if (!active(key, *record)) {
        AtomicSnapshotWriter::end(record->sequence, odd);
        unpin(key, *record);
        return false;
    }
    const u64 tick = now();
    const u32 previous_phase = record->phase.load<
        libk::MemoryOrder::Relaxed>();
    const u64 previous_semantic = record->semantic_stamp.load<
        libk::MemoryOrder::Relaxed>();
    increment_sat(record->activity_epoch, 1);
    libk::atomic_max(record->last_activity_at, tick);
    record->phase.store<libk::MemoryOrder::Relaxed>(phase);
    record->wait_kind.store<libk::MemoryOrder::Relaxed>(
        static_cast<u32>(wait));
    publish_node(
        record->driver_key,
        record->driver_kind,
        record->driver_generation,
        driver);
    publish_node(
        record->blocker_key,
        record->blocker_kind,
        record->blocker_generation,
        blocker);
    record->site_file.store<libk::MemoryOrder::Relaxed>(
        reinterpret_cast<usize>(site.file));
    record->site_function.store<libk::MemoryOrder::Relaxed>(
        reinterpret_cast<usize>(site.function));
    record->site_line.store<libk::MemoryOrder::Relaxed>(site.line);
    const bool progressed = update_progress
        && (previous_phase != phase
            || previous_semantic != semantic_stamp);
    if (update_progress) {
        record->semantic_stamp.store<libk::MemoryOrder::Relaxed>(
            semantic_stamp);
    }
    if (progressed) {
        increment_sat(record->progress_epoch, 1);
        libk::atomic_max(record->last_progress_at, tick);
    }
    AtomicSnapshotWriter::end(record->sequence, odd);
    unpin(key, *record);
    return true;
}

auto ObservationShard::write_batch(
    ObservationKey key,
    const ObservationBatch& update) noexcept -> bool {
    ObservationRecord* record{};
    if (!pin(key, record)) {
        return false;
    }
    u64 odd{};
    if (!AtomicSnapshotWriter::begin(record->sequence, odd)) {
        degrade_record(*record);
        mark_degraded(DiagnosticStatus::ObservationWriterCollision);
        degraded_.store<libk::MemoryOrder::Release>(1);
        unpin(key, *record);
        return false;
    }
    if (!active(key, *record)) {
        AtomicSnapshotWriter::end(record->sequence, odd);
        unpin(key, *record);
        return false;
    }
    const u64 tick = now();
    const u32 previous_phase = record->phase.load<
        libk::MemoryOrder::Relaxed>();
    const u64 previous_semantic = record->semantic_stamp.load<
        libk::MemoryOrder::Relaxed>();
    if (update.update_activity) {
        increment_sat(record->activity_epoch, 1);
        libk::atomic_max(record->last_activity_at, tick);
    }
    record->phase.store<libk::MemoryOrder::Relaxed>(update.phase);
    if (update.update_relation) {
        record->wait_kind.store<libk::MemoryOrder::Relaxed>(
            static_cast<u32>(update.wait));
        publish_node(
            record->driver_key,
            record->driver_kind,
            record->driver_generation,
            update.driver);
        publish_node(
            record->blocker_key,
            record->blocker_kind,
            record->blocker_generation,
            update.blocker);
    }
    if (update.update_deadline) {
        record->deadline.store<libk::MemoryOrder::Relaxed>(update.deadline);
        record->grace.store<libk::MemoryOrder::Relaxed>(update.grace);
    }
    record->site_file.store<libk::MemoryOrder::Relaxed>(
        reinterpret_cast<usize>(update.site.file));
    record->site_function.store<libk::MemoryOrder::Relaxed>(
        reinterpret_cast<usize>(update.site.function));
    record->site_line.store<libk::MemoryOrder::Relaxed>(update.site.line);
    // The semantic stamp is part of the coherent projection even when this
    // batch only republishes metadata after an independent witness advance.
    // update_progress controls the progress counter/timestamp, not whether
    // the new semantic state is visible.
    record->semantic_stamp.store<libk::MemoryOrder::Relaxed>(
        update.semantic_stamp);
    if (update.update_progress
        && (previous_phase != update.phase
            || previous_semantic != update.semantic_stamp)) {
        increment_sat(record->progress_epoch, 1);
        libk::atomic_max(record->last_progress_at, tick);
    }
    for (usize index = 0; index < 4; ++index) {
        if ((update.detail_mask & bit(index)) != 0) {
            record->detail[index].store<libk::MemoryOrder::Relaxed>(
                update.detail[index]);
        }
    }
    AtomicSnapshotWriter::end(record->sequence, odd);

    if (update.update_watched) {
        const u64 mask = bit(key.slot());
        if (update.watched) {
            static_cast<void>(watched_.fetch_or<libk::MemoryOrder::Release>(
                mask));
        } else {
            static_cast<void>(watched_.fetch_and<libk::MemoryOrder::Release>(
                ~mask));
        }
    }
    unpin(key, *record);
    return true;
}

auto ObservationShard::write_wait(
    ObservationKey key,
    ObservationKey wait,
    WaitKind kind,
    NodeRef driver,
    SourceSite site) noexcept -> bool {
    ObservationRecord* record{};
    if (!pin(key, record)) {
        return false;
    }
    u64 odd{};
    if (!AtomicSnapshotWriter::begin(record->sequence, odd)) {
        degrade_record(*record);
        mark_degraded(DiagnosticStatus::ObservationWriterCollision);
        degraded_.store<libk::MemoryOrder::Release>(1);
        unpin(key, *record);
        return false;
    }
    if (!active(key, *record)) {
        AtomicSnapshotWriter::end(record->sequence, odd);
        unpin(key, *record);
        return false;
    }
    const u64 tick = now();
    increment_sat(record->activity_epoch, 1);
    libk::atomic_max(record->last_activity_at, tick);
    record->wait_target.store<libk::MemoryOrder::Relaxed>(wait.raw);
    record->wait_kind.store<libk::MemoryOrder::Relaxed>(
        static_cast<u32>(kind));
    publish_node(
        record->driver_key,
        record->driver_kind,
        record->driver_generation,
        driver);
    record->site_file.store<libk::MemoryOrder::Relaxed>(
        reinterpret_cast<usize>(site.file));
    record->site_function.store<libk::MemoryOrder::Relaxed>(
        reinterpret_cast<usize>(site.function));
    record->site_line.store<libk::MemoryOrder::Relaxed>(site.line);
    AtomicSnapshotWriter::end(record->sequence, odd);
    unpin(key, *record);
    return true;
}

auto ObservationShard::write_policy(
    ObservationKey key,
    OperationPolicy policy,
    SourceSite site) noexcept -> bool {
    ObservationRecord* record{};
    if (!pin(key, record)) {
        return false;
    }
    u64 odd{};
    if (!AtomicSnapshotWriter::begin(record->sequence, odd)) {
        degrade_record(*record);
        mark_degraded(DiagnosticStatus::ObservationWriterCollision);
        degraded_.store<libk::MemoryOrder::Release>(1);
        unpin(key, *record);
        return false;
    }
    if (!active(key, *record)) {
        AtomicSnapshotWriter::end(record->sequence, odd);
        unpin(key, *record);
        return false;
    }
    const u32 kinds = static_cast<u32>(policy.kind)
        | (static_cast<u32>(policy.expectation) << 8)
        | (static_cast<u32>(policy.action) << 16)
        | (static_cast<u32>(policy.driver.kind) << 24);
    record->policy_kinds.store<libk::MemoryOrder::Relaxed>(kinds);
    record->policy_driver_key.store<libk::MemoryOrder::Relaxed>(
        policy.driver.identity);
    record->policy_driver_generation.store<libk::MemoryOrder::Relaxed>(
        policy.driver.generation);
    record->deadline.store<libk::MemoryOrder::Relaxed>(policy.deadline);
    record->grace.store<libk::MemoryOrder::Relaxed>(policy.grace);
    record->site_file.store<libk::MemoryOrder::Relaxed>(
        reinterpret_cast<usize>(site.file));
    record->site_function.store<libk::MemoryOrder::Relaxed>(
        reinterpret_cast<usize>(site.function));
    record->site_line.store<libk::MemoryOrder::Relaxed>(site.line);
    AtomicSnapshotWriter::end(record->sequence, odd);
    unpin(key, *record);
    return true;
}

auto ObservationShard::publish_operation(
    ObservationKey key,
    OperationPhase phase,
    NodeRef driver,
    NodeRef blocker,
    SourceSite site) noexcept -> bool {
    ObservationRecord* record{};
    if (!pin(key, record)) {
        return false;
    }
    u64 odd{};
    if (!AtomicSnapshotWriter::begin(record->sequence, odd)) {
        degrade_record(*record);
        mark_degraded(DiagnosticStatus::ObservationWriterCollision);
        degraded_.store<libk::MemoryOrder::Release>(1);
        unpin(key, *record);
        return false;
    }
    if (!active(key, *record)) {
        AtomicSnapshotWriter::end(record->sequence, odd);
        unpin(key, *record);
        return false;
    }

    WaitKind wait{};
    Expectation expectation{};
    bool watched{true};
    switch (phase) {
    case OperationPhase::Attached: {
        const u32 policy =
            record->policy_kinds.load<libk::MemoryOrder::Relaxed>();
        wait = static_cast<WaitKind>(policy & 0xffU);
        expectation = static_cast<Expectation>((policy >> 8) & 0xffU);
        watched = expectation != Expectation::ExternalUnbounded
            && expectation != Expectation::Idle
            && expectation != Expectation::ObserveOnly;
        break;
    }
    case OperationPhase::Claimed:
        wait = WaitKind::CompletionPublication;
        expectation = Expectation::InternalFinite;
        break;
    case OperationPhase::WakeIssued:
        wait = WaitKind::CompletionDelivery;
        expectation = Expectation::InternalFinite;
        break;
    case OperationPhase::ReadyPublished:
        wait = WaitKind::SchedulerReady;
        expectation = Expectation::SchedulerControlled;
        break;
    case OperationPhase::Finished:
    case OperationPhase::Cancelled:
        AtomicSnapshotWriter::end(record->sequence, odd);
        unpin(key, *record);
        return false;
    }

    const u64 tick = now();
    const u32 encoded_phase = static_cast<u32>(phase);
    const u32 previous_phase =
        record->phase.load<libk::MemoryOrder::Relaxed>();
    increment_sat(record->activity_epoch, 1);
    libk::atomic_max(record->last_activity_at, tick);
    record->phase.store<libk::MemoryOrder::Relaxed>(encoded_phase);
    record->wait_kind.store<libk::MemoryOrder::Relaxed>(
        static_cast<u32>(wait));
    record->expectation.store<libk::MemoryOrder::Relaxed>(
        static_cast<u32>(expectation));
    publish_node(
        record->driver_key,
        record->driver_kind,
        record->driver_generation,
        driver);
    publish_node(
        record->blocker_key,
        record->blocker_kind,
        record->blocker_generation,
        blocker);
    record->site_file.store<libk::MemoryOrder::Relaxed>(
        reinterpret_cast<usize>(site.file));
    record->site_function.store<libk::MemoryOrder::Relaxed>(
        reinterpret_cast<usize>(site.function));
    record->site_line.store<libk::MemoryOrder::Relaxed>(site.line);
    record->semantic_stamp.store<libk::MemoryOrder::Relaxed>(encoded_phase);
    if (previous_phase != encoded_phase) {
        increment_sat(record->progress_epoch, 1);
        libk::atomic_max(record->last_progress_at, tick);
    }
    AtomicSnapshotWriter::end(record->sequence, odd);

    const u64 mask = bit(key.slot());
    if (watched) {
        static_cast<void>(watched_.fetch_or<libk::MemoryOrder::Release>(mask));
    } else {
        static_cast<void>(watched_.fetch_and<libk::MemoryOrder::Release>(
            ~mask));
    }
    unpin(key, *record);
    return true;
}

void ObservationShard::write_deadline(
    ObservationKey key,
    u64 absolute,
    u64 grace,
    SourceSite site) noexcept {
    ObservationRecord* record{};
    if (!pin(key, record)) {
        return;
    }
    u64 odd{};
    if (!AtomicSnapshotWriter::begin(record->sequence, odd)) {
        degrade_record(*record);
        mark_degraded(DiagnosticStatus::ObservationWriterCollision);
        degraded_.store<libk::MemoryOrder::Release>(1);
        unpin(key, *record);
        return;
    }
    if (active(key, *record)) {
        record->deadline.store<libk::MemoryOrder::Relaxed>(absolute);
        record->grace.store<libk::MemoryOrder::Relaxed>(grace);
        record->site_file.store<libk::MemoryOrder::Relaxed>(
            reinterpret_cast<usize>(site.file));
        record->site_function.store<libk::MemoryOrder::Relaxed>(
            reinterpret_cast<usize>(site.function));
        record->site_line.store<libk::MemoryOrder::Relaxed>(site.line);
    }
    AtomicSnapshotWriter::end(record->sequence, odd);
    unpin(key, *record);
}

void ObservationShard::set_watched(
    ObservationKey key,
    bool watched) noexcept {
    ObservationRecord* record{};
    if (!pin(key, record)) {
        return;
    }
    const usize index = key.slot();
    usize local{};
    ObservationPage* const page = page_for(index, local);
    if (page == nullptr) {
        unpin(key, *record);
        return;
    }
    const u64 mask = bit(index);
    if (watched) {
        static_cast<void>(watched_.fetch_or<libk::MemoryOrder::Release>(mask));
    } else {
        static_cast<void>(watched_.fetch_and<libk::MemoryOrder::Release>(~mask));
    }
    unpin(key, *record);
}

auto ObservationShard::update_phase(
    ObservationKey key,
    u32 phase,
    u64 semantic_stamp,
    SourceSite site) noexcept -> bool {
    ObservationRecord* record{};
    if (!pin(key, record)) {
        return false;
    }
    u64 odd{};
    if (!AtomicSnapshotWriter::begin(record->sequence, odd)) {
        degrade_record(*record);
        mark_degraded(DiagnosticStatus::ObservationWriterCollision);
        degraded_.store<libk::MemoryOrder::Release>(1);
        unpin(key, *record);
        return false;
    }
    if (!active(key, *record)) {
        AtomicSnapshotWriter::end(record->sequence, odd);
        unpin(key, *record);
        return false;
    }
    const u64 tick = now();
    const u32 previous_phase = record->phase.load<
        libk::MemoryOrder::Relaxed>();
    const u64 previous_semantic = record->semantic_stamp.load<
        libk::MemoryOrder::Relaxed>();
    increment_sat(record->activity_epoch, 1);
    libk::atomic_max(record->last_activity_at, tick);
    record->phase.store<libk::MemoryOrder::Relaxed>(phase);
    record->site_file.store<libk::MemoryOrder::Relaxed>(
        reinterpret_cast<usize>(site.file));
    record->site_function.store<libk::MemoryOrder::Relaxed>(
        reinterpret_cast<usize>(site.function));
    record->site_line.store<libk::MemoryOrder::Relaxed>(site.line);
    record->semantic_stamp.store<libk::MemoryOrder::Relaxed>(semantic_stamp);
    if (previous_phase != phase || previous_semantic != semantic_stamp) {
        increment_sat(record->progress_epoch, 1);
        libk::atomic_max(record->last_progress_at, tick);
    }
    AtomicSnapshotWriter::end(record->sequence, odd);
    unpin(key, *record);
    return true;
}

auto ObservationShard::update_progress(
    ObservationKey key,
    u64 semantic_stamp,
    bool force) noexcept -> bool {
    ObservationRecord* record{};
    if (!pin(key, record)) {
        return false;
    }
    u64 odd{};
    if (!AtomicSnapshotWriter::begin(record->sequence, odd)) {
        degrade_record(*record);
        mark_degraded(DiagnosticStatus::ObservationWriterCollision);
        degraded_.store<libk::MemoryOrder::Release>(1);
        unpin(key, *record);
        return false;
    }
    if (!active(key, *record)) {
        AtomicSnapshotWriter::end(record->sequence, odd);
        unpin(key, *record);
        return false;
    }
    const u64 previous = record->semantic_stamp.load<
        libk::MemoryOrder::Relaxed>();
    const bool changed = force || previous != semantic_stamp;
    if (changed) {
        record->semantic_stamp.store<libk::MemoryOrder::Relaxed>(
            semantic_stamp);
        increment_sat(record->progress_epoch, 1);
        libk::atomic_max(record->last_progress_at, now());
    }
    AtomicSnapshotWriter::end(record->sequence, odd);
    unpin(key, *record);
    return changed;
}

auto ObservationShard::observe(
    ObservationKey key,
    u64 semantic_stamp) noexcept -> bool {
    ObservationRecord* record{};
    if (!pin(key, record)) {
        return false;
    }
    u64 odd{};
    if (!AtomicSnapshotWriter::begin(record->sequence, odd)) {
        degrade_record(*record);
        mark_degraded(DiagnosticStatus::ObservationWriterCollision);
        degraded_.store<libk::MemoryOrder::Release>(1);
        unpin(key, *record);
        return false;
    }
    if (!active(key, *record)) {
        AtomicSnapshotWriter::end(record->sequence, odd);
        unpin(key, *record);
        return false;
    }
    const u64 tick = now();
    const u64 previous = record->semantic_stamp.load<
        libk::MemoryOrder::Relaxed>();
    increment_sat(record->activity_epoch, 1);
    libk::atomic_max(record->last_activity_at, tick);
    if (previous != semantic_stamp) {
        record->semantic_stamp.store<libk::MemoryOrder::Relaxed>(
            semantic_stamp);
        increment_sat(record->progress_epoch, 1);
        libk::atomic_max(record->last_progress_at, tick);
    }
    AtomicSnapshotWriter::end(record->sequence, odd);
    unpin(key, *record);
    return previous != semantic_stamp;
}

void ObservationShard::update_activity(
    ObservationKey key,
    u64 delta) noexcept {
    ObservationRecord* record{};
    if (delta == 0 || !pin(key, record)) {
        return;
    }
    if (!active(key, *record)) {
        unpin(key, *record);
        return;
    }
    increment_sat(record->activity_epoch, delta);
    libk::atomic_max(record->last_activity_at, now());
    unpin(key, *record);
}

void ObservationShard::advance(
    ObservationKey key,
    u64 delta) noexcept {
    ObservationRecord* record{};
    if (delta == 0 || !pin(key, record)) {
        return;
    }
    if (!active(key, *record)) {
        unpin(key, *record);
        return;
    }
    // A direct advance is a progress-only witness. Progress changes reset
    // the watchdog candidate; metadata transactions may publish activity and
    // progress together, but no split pair can be manufactured here because
    // this direct path does not write activity_epoch.
    const u64 tick = now();
    increment_sat(record->progress_epoch, delta);
    libk::atomic_max(record->last_progress_at, tick);
    unpin(key, *record);
}

void ObservationShard::write_detail(
    ObservationKey key,
    usize index,
    u64 value) noexcept {
    ObservationRecord* record{};
    if (index >= 4 || !pin(key, record)) {
        return;
    }
    if (active(key, *record)) {
        record->detail[index].store<libk::MemoryOrder::Relaxed>(value);
    }
    unpin(key, *record);
}

void ObservationShard::and_detail(
    ObservationKey key,
    usize index,
    u64 mask) noexcept {
    ObservationRecord* record{};
    if (index >= 4 || !pin(key, record)) {
        return;
    }
    if (active(key, *record)) {
        static_cast<void>(record->detail[index].fetch_and<
            libk::MemoryOrder::Relaxed>(mask));
    }
    unpin(key, *record);
}

auto ObservationShard::snapshot(
    ObservationKey key,
    ObservationSnapshot& result) const noexcept -> bool {
    ObservationRecord* record{};
    if (!pin(key, record)) {
        return false;
    }
    for (usize attempt = 0; attempt < 3; ++attempt) {
        const u64 first = AtomicSnapshotReader::begin(record->sequence);
        if ((first & 1U) != 0) {
            continue;
        }
        ObservationSnapshot value{};
        value.generation = key.generation();
        value.activity_epoch = record->activity_epoch.load<libk::MemoryOrder::Relaxed>();
        value.progress_epoch = record->progress_epoch.load<libk::MemoryOrder::Relaxed>();
        value.started_at = record->started_at.load<libk::MemoryOrder::Relaxed>();
        value.last_activity_at = record->last_activity_at.load<libk::MemoryOrder::Relaxed>();
        value.last_progress_at = record->last_progress_at.load<libk::MemoryOrder::Relaxed>();
        value.evidence = static_cast<EvidenceGrade>(record->evidence.load<
            libk::MemoryOrder::Acquire>());
        value.record_kind = static_cast<RecordKind>(
            record->record_kind.load<libk::MemoryOrder::Relaxed>());
        value.phase = record->phase.load<libk::MemoryOrder::Relaxed>();
        value.wait_kind = static_cast<WaitKind>(
            record->wait_kind.load<libk::MemoryOrder::Relaxed>());
        value.expectation = static_cast<Expectation>(
            record->expectation.load<libk::MemoryOrder::Relaxed>());
        const u32 policy =
            record->policy_kinds.load<libk::MemoryOrder::Relaxed>();
        value.policy.kind = static_cast<WaitKind>(policy & 0xffU);
        value.policy.expectation =
            static_cast<Expectation>((policy >> 8) & 0xffU);
        value.policy.action =
            static_cast<StallAction>((policy >> 16) & 0xffU);
        value.policy.driver = read_node(
            record->policy_driver_key.load<libk::MemoryOrder::Relaxed>(),
            (policy >> 24) & 0xffU,
            record->policy_driver_generation.load<
                libk::MemoryOrder::Relaxed>());
        value.policy.deadline =
            record->deadline.load<libk::MemoryOrder::Relaxed>();
        value.policy.grace =
            record->grace.load<libk::MemoryOrder::Relaxed>();
        value.subject_identity = record->subject_identity.load<libk::MemoryOrder::Relaxed>();
        value.subject_generation = record->subject_generation.load<libk::MemoryOrder::Relaxed>();
        value.wait_target = ObservationKey{
            record->wait_target.load<libk::MemoryOrder::Relaxed>()};
        value.driver = read_node(
            record->driver_key.load<libk::MemoryOrder::Relaxed>(),
            record->driver_kind.load<libk::MemoryOrder::Relaxed>(),
            record->driver_generation.load<libk::MemoryOrder::Relaxed>());
        if (value.expectation == Expectation::DeadlineBound) {
            // Completion owns the full immutable policy.  While that policy
            // is active, the live Attached relation is the timeout source;
            // deriving it here avoids a second copy in every observation.
            value.policy.deadline_driver = value.driver;
        }
        value.blocker = read_node(
            record->blocker_key.load<libk::MemoryOrder::Relaxed>(),
            record->blocker_kind.load<libk::MemoryOrder::Relaxed>(),
            record->blocker_generation.load<libk::MemoryOrder::Relaxed>());
        value.semantic_stamp = record->semantic_stamp.load<libk::MemoryOrder::Relaxed>();
        value.site.file = reinterpret_cast<const char*>(
            record->site_file.load<libk::MemoryOrder::Relaxed>());
        value.site.function = reinterpret_cast<const char*>(
            record->site_function.load<libk::MemoryOrder::Relaxed>());
        value.site.line = record->site_line.load<libk::MemoryOrder::Relaxed>();
        for (usize index = 0; index < 4; ++index) {
            value.detail[index] = record->detail[index].load<
                libk::MemoryOrder::Relaxed>();
        }
        if (AtomicSnapshotReader::valid(record->sequence, first)
            && active(key, *record)) {
            result = value;
            unpin(key, *record);
            return true;
        }
    }
    unpin(key, *record);
    // A failed reader sample is inconclusive for this generation; retain only
    // cumulative shard health. Generation-local degradation belongs to writer
    // publication loss, not transient reader overlap.
    const_cast<ObservationShard*>(this)->mark_degraded(
        DiagnosticStatus::SnapshotUnstable);
    return false;
}

void ObservationShard::finish(
    ObservationKey key,
    u32 terminal_phase,
    u64 result,
    SourceSite site) noexcept {
    ObservationRecord* record{};
    if (!pin(key, record)) {
        return;
    }

    const u64 tick = now();
    const RecordKind kind = static_cast<RecordKind>(record->record_kind.load<
        libk::MemoryOrder::Acquire>());
    const u64 started = record->started_at.load<libk::MemoryOrder::Acquire>();
    u64 odd{};
    if (AtomicSnapshotWriter::begin(record->sequence, odd)) {
        if (!retire(key, *record)) {
            AtomicSnapshotWriter::end(record->sequence, odd);
            unpin(key, *record);
            return;
        }
        increment_sat(record->activity_epoch, 1);
        libk::atomic_max(record->last_activity_at, tick);
        record->phase.store<libk::MemoryOrder::Relaxed>(terminal_phase);
        record->wait_kind.store<libk::MemoryOrder::Relaxed>(
            static_cast<u32>(WaitKind::None));
        record->wait_target.store<libk::MemoryOrder::Relaxed>(0);
        publish_node(
            record->driver_key,
            record->driver_kind,
            record->driver_generation,
            {});
        publish_node(
            record->blocker_key,
            record->blocker_kind,
            record->blocker_generation,
            {});
        record->site_file.store<libk::MemoryOrder::Relaxed>(
            reinterpret_cast<usize>(site.file));
        record->site_function.store<libk::MemoryOrder::Relaxed>(
            reinterpret_cast<usize>(site.function));
        record->site_line.store<libk::MemoryOrder::Relaxed>(site.line);
        record->detail[0].store<libk::MemoryOrder::Relaxed>(result);
        record->semantic_stamp.store<libk::MemoryOrder::Relaxed>(result);
        increment_sat(record->progress_epoch, 1);
        libk::atomic_max(record->last_progress_at, tick);
        AtomicSnapshotWriter::end(record->sequence, odd);
        profile_finish(kind, started != 0 ? elapsed(tick, started) : 0);
    } else {
        degrade_record(*record);
        mark_degraded(DiagnosticStatus::ObservationWriterCollision);
        degraded_.store<libk::MemoryOrder::Release>(1);
        // A colliding writer already holds a pin and may complete its
        // pre-retirement update. Closing Active still prevents any new
        // borrower from reopening the terminal generation.
        static_cast<void>(retire(key, *record));
    }
    unpin(key, *record);
}

auto ObservationLease::resolve(
    ObservationShard*& shard,
    ObservationRecord*& record) const noexcept -> bool {
#if MYOS_CONCURRENCY_DIAG >= 1
    shard = nullptr;
    record = nullptr;
    if (!key_) {
        return false;
    }
    void* const owner = arch::current_cpu_owner();
    if (owner == nullptr) {
        return false;
    }
    auto& cpu = *static_cast<CpuLocal*>(owner);
    if (cpu.runtime_ == nullptr || cpu.runtime_->owner_registry == nullptr) {
        return false;
    }
    CpuRegistry* const registry = cpu.runtime_->owner_registry;
    CpuRuntime* const target = registry->runtime(key_.shard());
    shard = target == nullptr || target->diagnostics == nullptr
        || target->diagnostics->concurrency == nullptr
        ? nullptr : target->diagnostics->concurrency->observations;
    if (shard == nullptr) {
        return false;
    }
    record = shard->valid(key_);
    if (record != nullptr) {
        return true;
    }
    return false;
#else
    static_cast<void>(shard);
    static_cast<void>(record);
    return false;
#endif
}

ObservationLease::~ObservationLease() noexcept {
    reset();
}

ObservationLease::ObservationLease(ObservationLease&& other) noexcept
#if MYOS_CONCURRENCY_DIAG >= 1
    : key_(libk::exchange(other.key_, {})),
      owned_(libk::exchange(other.owned_, false))
{
}
#else
{
    static_cast<void>(other);
}
#endif

auto ObservationLease::operator=(ObservationLease&& other) noexcept
    -> ObservationLease& {
    if (this != &other) {
        reset();
#if MYOS_CONCURRENCY_DIAG >= 1
        key_ = libk::exchange(other.key_, {});
        owned_ = libk::exchange(other.owned_, false);
#else
        static_cast<void>(other);
#endif
    }
    return *this;
}

ObservationLease::operator bool() const noexcept {
#if MYOS_CONCURRENCY_DIAG >= 1
    ObservationShard* shard{};
    ObservationRecord* record{};
    return resolve(shard, record);
#else
    return false;
#endif
}

auto ObservationLease::key() const noexcept -> ObservationKey {
#if MYOS_CONCURRENCY_DIAG >= 1
    return key_;
#else
    return {};
#endif
}

auto ObservationLease::reserve(
    RecordKind kind,
    u64 subject_identity,
    u64 subject_generation,
    Expectation expectation,
    SourceSite site) noexcept -> ObservationLease {
    return concurrency::reserve(
        kind, subject_identity, subject_generation, expectation, site);
}

auto ObservationLease::reserve_on(
    CpuId cpu,
    RecordKind kind,
    u64 subject_identity,
    u64 subject_generation,
    Expectation expectation,
    SourceSite site) noexcept -> ObservationLease {
    return concurrency::reserve_on(
        cpu,
        kind,
        subject_identity,
        subject_generation,
        expectation,
        site);
}

auto ObservationLease::borrow(ObservationKey key) noexcept
    -> ObservationLease {
#if MYOS_CONCURRENCY_DIAG >= 1
    if (!key) {
        return {};
    }
    ObservationLease result{key, false};
    return result ? libk::move(result) : ObservationLease{};
#else
    static_cast<void>(key);
    return {};
#endif
}

auto ObservationLease::detach_key() noexcept -> ObservationKey {
#if MYOS_CONCURRENCY_DIAG >= 1
    if (!*this) {
        return {};
    }
    const ObservationKey result = key_;
    owned_ = false;
    key_ = {};
    return result;
#else
    return {};
#endif
}

void ObservationLease::attempt(
    u32 phase,
    WaitKind wait,
    NodeRef driver,
    NodeRef blocker,
    SourceSite site) noexcept {
#if MYOS_CONCURRENCY_DIAG >= 1
    ObservationShard* shard{};
    ObservationRecord* record{};
    if (!resolve(shard, record)) {
        return;
    }
    static_cast<void>(shard->write_metadata(
        key_, phase, wait, driver, blocker, 0, false, site));
#else
    static_cast<void>(phase);
    static_cast<void>(wait);
    static_cast<void>(driver);
    static_cast<void>(blocker);
    static_cast<void>(site);
#endif
}

void ObservationLease::transition(
    u32 phase,
    u64 semantic_stamp,
    WaitKind wait,
    NodeRef driver,
    NodeRef blocker,
    SourceSite site) noexcept {
#if MYOS_CONCURRENCY_DIAG >= 1
    ObservationShard* shard{};
    ObservationRecord* record{};
    if (resolve(shard, record)) {
        static_cast<void>(shard->write_metadata(
            key_, phase, wait, driver, blocker, semantic_stamp, true, site));
    }
#else
    static_cast<void>(phase);
    static_cast<void>(semantic_stamp);
    static_cast<void>(wait);
    static_cast<void>(driver);
    static_cast<void>(blocker);
    static_cast<void>(site);
#endif
}

void ObservationLease::set_policy(
    OperationPolicy policy,
    SourceSite site) noexcept {
#if MYOS_CONCURRENCY_DIAG >= 1
    ObservationShard* shard{};
    ObservationRecord* record{};
    if (resolve(shard, record)) {
        static_cast<void>(shard->write_policy(key_, policy, site));
    }
#else
    static_cast<void>(policy);
    static_cast<void>(site);
#endif
}

void ObservationLease::publish(
    OperationPhase phase,
    NodeRef driver,
    NodeRef blocker,
    SourceSite site) noexcept {
#if MYOS_CONCURRENCY_DIAG >= 1
    ObservationShard* shard{};
    ObservationRecord* record{};
    if (resolve(shard, record)) {
        static_cast<void>(shard->publish_operation(
            key_, phase, driver, blocker, site));
    }
#else
    static_cast<void>(phase);
    static_cast<void>(driver);
    static_cast<void>(blocker);
    static_cast<void>(site);
#endif
}

void ObservationLease::deadline(
    u64 absolute,
    u64 grace,
    SourceSite site) noexcept {
#if MYOS_CONCURRENCY_DIAG >= 1
    ObservationShard* shard{};
    ObservationRecord* record{};
    if (resolve(shard, record)) {
        shard->write_deadline(key_, absolute, grace, site);
    }
#else
    static_cast<void>(absolute);
    static_cast<void>(grace);
    static_cast<void>(site);
#endif
}

void ObservationLease::phase(
    u32 phase,
    u64 semantic_stamp,
    SourceSite site) noexcept {
#if MYOS_CONCURRENCY_DIAG >= 1
    ObservationShard* shard{};
    ObservationRecord* record{};
    if (!resolve(shard, record)) {
        return;
    }
    static_cast<void>(shard->update_phase(key_, phase, semantic_stamp, site));
#else
    static_cast<void>(phase);
    static_cast<void>(semantic_stamp);
    static_cast<void>(site);
#endif
}

void ObservationLease::observe(u64 semantic_stamp) noexcept {
#if MYOS_CONCURRENCY_DIAG >= 1
    ObservationShard* shard{};
    ObservationRecord* record{};
    if (!resolve(shard, record)) {
        return;
    }
    static_cast<void>(shard->observe(key_, semantic_stamp));
#else
    static_cast<void>(semantic_stamp);
#endif
}

void ObservationLease::touch(SourceSite site) noexcept {
#if MYOS_CONCURRENCY_DIAG >= 1
    ObservationShard* shard{};
    ObservationRecord* record{};
    if (!resolve(shard, record)) {
        return;
    }
    shard->update_activity(key_, 1);
    static_cast<void>(site);
#else
    static_cast<void>(site);
#endif
}

void ObservationLease::advance(u64 delta) noexcept {
#if MYOS_CONCURRENCY_DIAG >= 1
    ObservationShard* shard{};
    ObservationRecord* record{};
    if (delta == 0 || !resolve(shard, record)) {
        return;
    }
    shard->advance(key_, delta);
#else
    static_cast<void>(delta);
#endif
}

void ObservationLease::detail(usize index, u64 value) noexcept {
#if MYOS_CONCURRENCY_DIAG >= 1
    ObservationShard* shard{};
    ObservationRecord* record{};
    if (!resolve(shard, record)) {
        return;
    }
    shard->write_detail(key_, index, value);
#else
    static_cast<void>(index);
    static_cast<void>(value);
#endif
}

void ObservationLease::detail_and(usize index, u64 mask) noexcept {
#if MYOS_CONCURRENCY_DIAG >= 1
    ObservationShard* shard{};
    ObservationRecord* record{};
    if (!resolve(shard, record)) {
        return;
    }
    shard->and_detail(key_, index, mask);
#else
    static_cast<void>(index);
    static_cast<void>(mask);
#endif
}

void ObservationLease::publish(const ObservationBatch& update) noexcept {
#if MYOS_CONCURRENCY_DIAG >= 1
    ObservationShard* shard{};
    ObservationRecord* record{};
    if (resolve(shard, record)) {
        static_cast<void>(shard->write_batch(key_, update));
    }
#else
    static_cast<void>(update);
#endif
}

void ObservationLease::link_wait(
    ObservationKey wait,
    WaitKind kind,
    NodeRef driver,
    SourceSite site) noexcept {
#if MYOS_CONCURRENCY_DIAG >= 1
    ObservationShard* shard{};
    ObservationRecord* record{};
    if (!resolve(shard, record)) {
        return;
    }
    if (shard->write_wait(key_, wait, kind, driver, site)) {
        shard->set_watched(key_, static_cast<bool>(wait));
    }
#else
    static_cast<void>(wait);
    static_cast<void>(kind);
    static_cast<void>(driver);
    static_cast<void>(site);
#endif
}

void ObservationLease::clear_wait(SourceSite site) noexcept {
#if MYOS_CONCURRENCY_DIAG >= 1
    ObservationShard* shard{};
    ObservationRecord* record{};
    if (!resolve(shard, record)) {
        return;
    }
    if (shard->write_wait(key_, {}, WaitKind::None, {}, site)) {
        shard->set_watched(key_, false);
    }
#else
    static_cast<void>(site);
#endif
}

void ObservationLease::watch(bool enabled) noexcept {
#if MYOS_CONCURRENCY_DIAG >= 1
    ObservationShard* shard{};
    ObservationRecord* record{};
    if (resolve(shard, record)) {
        shard->set_watched(key_, enabled);
    }
#else
    static_cast<void>(enabled);
#endif
}

void ObservationLease::finish(
    u32 terminal_phase,
    u64 result,
    SourceSite site) noexcept {
#if MYOS_CONCURRENCY_DIAG >= 1
    ObservationShard* shard{};
    ObservationRecord* record{};
    if (!resolve(shard, record)) {
        return;
    }
    shard->finish(key_, terminal_phase, result, site);
    key_ = {};
    owned_ = false;
#else
    static_cast<void>(terminal_phase);
    static_cast<void>(result);
    static_cast<void>(site);
#endif
}

auto ObservationLease::snapshot(
    ObservationSnapshot& result) const noexcept -> bool {
#if MYOS_CONCURRENCY_DIAG >= 1
    ObservationShard* shard{};
    ObservationRecord* record{};
    return resolve(shard, record) && shard->snapshot(key_, result);
#else
    static_cast<void>(result);
    return false;
#endif
}

void ObservationLease::reset() noexcept {
#if MYOS_CONCURRENCY_DIAG >= 1
    if (owned_) {
        ObservationShard* shard{};
        ObservationRecord* record{};
        if (resolve(shard, record)) {
            shard->release(key_);
        }
    }
    key_ = {};
    owned_ = false;
#endif
}

static auto current_core() noexcept -> CpuDiagnosticsCore* {
#if MYOS_CONCURRENCY_DIAG >= 1
    void* const owner = arch::current_cpu_owner();
    if (owner == nullptr) {
        return nullptr;
    }
    auto& cpu = *static_cast<CpuLocal*>(owner);
    if (cpu.runtime_ == nullptr || cpu.runtime_->diagnostics == nullptr) {
        return nullptr;
    }
    return cpu.runtime_->diagnostics->concurrency;
#else
    return nullptr;
#endif
}

auto ReportNotifier::install(ReportCallback notifier) noexcept -> bool {
#if MYOS_CONCURRENCY_DIAG >= 3
    if (state_.load<libk::MemoryOrder::Acquire>() != 0) {
        // Binding is an initialization operation, not a callback swap.  A
        // repeated bind must leave both the live callback and any teardown
        // readers untouched.
        return false;
    }
    callback_ = notifier;
    state_.store<libk::MemoryOrder::Release>(bound_bit);
    return true;
#else
    static_cast<void>(notifier);
    return false;
#endif
}

void ReportNotifier::unbind() noexcept {
#if MYOS_CONCURRENCY_DIAG >= 3
    static_cast<void>(state_.fetch_and<libk::MemoryOrder::AcqRel>(
        reader_mask));
    while ((state_.load<libk::MemoryOrder::Acquire>() & reader_mask) != 0) {
        libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
    }
    callback_.reset();
#endif
}

auto ReportNotifier::notify() noexcept -> bool {
#if MYOS_CONCURRENCY_DIAG >= 3
    if (!::kernel::diag::scenario::report_notify_gate()) {
        return false;
    }
    u64 observed = state_.load<libk::MemoryOrder::Acquire>();
    if ((observed & bound_bit) == 0
        || (observed & reader_mask) == reader_mask) {
        return false;
    }
    // A single strong CAS is intentional.  The producer's contract is
    // best-effort; contention or an unbind is surfaced as a false result
    // rather than turned into a hidden spin.
    if (!state_.compare_exchange_strong<
            libk::MemoryOrder::AcqRel,
            libk::MemoryOrder::Acquire>(
                observed, observed + reader_bit)) {
        return false;
    }
    const bool notified = callback_ && callback_();
    static_cast<void>(state_.fetch_sub<libk::MemoryOrder::Release>(
        reader_bit));
    return notified;
#else
    return false;
#endif
}

auto bind_report_notifier(ReportCallback notifier) noexcept -> bool {
#if MYOS_CONCURRENCY_DIAG >= 3
    return report_notifier.install(notifier);
#else
    static_cast<void>(notifier);
    return false;
#endif
}

void unbind_report_notifier() noexcept {
#if MYOS_CONCURRENCY_DIAG >= 3
    report_notifier.unbind();
#endif
}


auto drain_reports(CpuRegistry& registry) noexcept -> usize {
#if MYOS_CONCURRENCY_DIAG >= 3
    // Report output is deliberately restricted to a normal kernel-thread
    // context. The watchdog producer is IRQ-off and never calls this path.
    if (!arch::interrupts_enabled()) {
        return 0;
    }
    usize drained{};
    for (usize index = 0; index < registry.count(); ++index) {
        const CpuRuntime* const runtime = registry.runtime(CpuId{index});
        if (runtime == nullptr || runtime->diagnostics == nullptr) {
            continue;
        }
        auto& queue = runtime->diagnostics->concurrency->reports;
        ReportRecord report{};
        while (queue.consume(report)) {
            ++drained;
            diag::console::print<
                "[concurrency] report watcher={} target={} "
                "root-kind={} root={:#x} state={} class={} evidence={} "
                "age={} nodes={} truncated={} pending={}/{} omitted={}\n">(
                report.watcher.raw,
                report.target.raw,
                static_cast<u32>(report.root.kind),
                report.root.identity,
                static_cast<u32>(report.state),
                stall_class_name(report.classification),
                static_cast<u32>(report.evidence),
                report.age,
                report.graph.count,
                report.graph.truncated,
                report.graph.pending_shown,
                report.graph.pending_total,
                report.graph.pending_omitted);
            if (report.root_snapshot_valid) {
                const auto& snapshot = report.root_snapshot;
                diag::console::print<
                    "  root-snapshot gen={} kind={} phase={} wait={} "
                    "expectation={} subject={:#x} subject-gen={} "
                    "activity={} progress={} started={} last-activity={} "
                    "last-progress={} evidence={} semantic={}\n">(
                    snapshot.generation,
                    static_cast<u32>(snapshot.record_kind),
                    snapshot.phase,
                    static_cast<u32>(snapshot.wait_kind),
                    static_cast<u32>(snapshot.expectation),
                    snapshot.subject_identity,
                    snapshot.subject_generation,
                    snapshot.activity_epoch,
                    snapshot.progress_epoch,
                    snapshot.started_at,
                    snapshot.last_activity_at,
                    snapshot.last_progress_at,
                    static_cast<u32>(snapshot.evidence),
                    snapshot.semantic_stamp);
                diag::console::print<
                    "  root-relation wait-key={:#x} driver-kind={} "
                    "driver={:#x} driver-gen={} blocker-kind={} "
                    "blocker={:#x} blocker-gen={} site={}:{} function={} "
                    "detail={:#x},{:#x},{:#x},{:#x}\n">(
                    snapshot.wait_target.raw,
                    static_cast<u32>(snapshot.driver.kind),
                    snapshot.driver.identity,
                    snapshot.driver.generation,
                    static_cast<u32>(snapshot.blocker.kind),
                    snapshot.blocker.identity,
                    snapshot.blocker.generation,
                    snapshot.site.file,
                    snapshot.site.line,
                    snapshot.site.function,
                    snapshot.detail[0],
                    snapshot.detail[1],
                    snapshot.detail[2],
                    snapshot.detail[3]);
                diag::console::print<
                    "  root-policy kind={} expectation={} action={} "
                    "driver-kind={} driver={:#x} driver-gen={} "
                    "deadline-driver-kind={} deadline-driver={:#x} "
                    "deadline-driver-gen={} deadline={} grace={}\n">(
                    static_cast<u32>(snapshot.policy.kind),
                    static_cast<u32>(snapshot.policy.expectation),
                    static_cast<u32>(snapshot.policy.action),
                    static_cast<u32>(snapshot.policy.driver.kind),
                    snapshot.policy.driver.identity,
                    snapshot.policy.driver.generation,
                    static_cast<u32>(snapshot.policy.deadline_driver.kind),
                    snapshot.policy.deadline_driver.identity,
                    snapshot.policy.deadline_driver.generation,
                    snapshot.policy.deadline,
                    snapshot.policy.grace);
            } else {
                diag::console::print<"  root-snapshot unavailable\n">();
            }
            for (usize node = 0; node < report.graph.count; ++node) {
                const NodeRef value = report.graph.node(node);
                diag::console::print<
                    "  graph[{}] parent={} edge={} kind={} node={:#x} "
                    "gen={}\n">(
                    node,
                    report.graph.parents[node],
                    static_cast<u32>(report.graph.edge(node)),
                    static_cast<u32>(value.kind),
                    value.identity,
                    value.generation);
            }
        }
    }
    return drained;
#else
    static_cast<void>(registry);
    return 0;
#endif
}

[[maybe_unused]] static auto current_shard() noexcept -> ObservationShard* {
#if MYOS_CONCURRENCY_DIAG >= 1
    CpuDiagnosticsCore* const core = current_core();
    return core != nullptr ? core->observations : nullptr;
#else
    return nullptr;
#endif
}

auto reserve(
    RecordKind kind,
    u64 subject_identity,
    u64 subject_generation,
    Expectation expectation,
    SourceSite site) noexcept -> ObservationLease {
#if MYOS_CONCURRENCY_DIAG >= 1
    ObservationShard* const shard = current_shard();
    if (shard == nullptr) {
        mark_degraded(DiagnosticStatus::StorageMissing);
        return {};
    }
    return shard->reserve(
        kind, subject_identity, subject_generation, expectation, site);
#else
    static_cast<void>(kind);
    static_cast<void>(subject_identity);
    static_cast<void>(subject_generation);
    static_cast<void>(expectation);
    static_cast<void>(site);
    return {};
#endif
}

auto reserve_on(
    CpuId cpu,
    RecordKind kind,
    u64 subject_identity,
    u64 subject_generation,
    Expectation expectation,
    SourceSite site) noexcept -> ObservationLease {
#if MYOS_CONCURRENCY_DIAG >= 1
    void* const owner = arch::current_cpu_owner();
    if (owner == nullptr) {
        mark_degraded(DiagnosticStatus::StorageMissing);
        return {};
    }
    auto& local = *static_cast<CpuLocal*>(owner);
    CpuRegistry* const registry = local.runtime_ == nullptr
        ? nullptr : local.runtime_->owner_registry;
    if (registry == nullptr) {
        const CpuId current = local.descriptor == nullptr
            ? CpuId{} : local.descriptor->logical_id();
        if (current != cpu) {
            mark_degraded(DiagnosticStatus::RemoteShardUnavailable);
            return {};
        }
        return reserve(
            kind, subject_identity, subject_generation, expectation, site);
    }
    CpuRuntime* const target = registry->runtime(cpu);
    ObservationShard* const shard = target == nullptr
        || target->diagnostics == nullptr
        || target->diagnostics->concurrency == nullptr
        ? nullptr : target->diagnostics->concurrency->observations;
    if (shard == nullptr) {
        mark_degraded(DiagnosticStatus::RemoteShardUnavailable);
        return {};
    }
    return shard->reserve(
        kind, subject_identity, subject_generation, expectation, site);
#else
    static_cast<void>(cpu);
    static_cast<void>(kind);
    static_cast<void>(subject_identity);
    static_cast<void>(subject_generation);
    static_cast<void>(expectation);
    static_cast<void>(site);
    return {};
#endif
}


void record(
    FlightDomain domain,
    FlightEvent event,
    u64 actor,
    u64 subject,
    u64 arg0,
    u64 arg1,
    u64 arg2,
    SourceSite site) noexcept {
#if MYOS_CONCURRENCY_DIAG >= 2
    CpuDiagnosticsCore* const core = current_core();
    if (core == nullptr || core->flight == nullptr) {
        return;
    }
    const u64 tick = now();
    core->live.last_event_at.store<libk::MemoryOrder::Release>(tick);
    core->flight->push(
        tick, domain, event, actor, subject, arg0, arg1, arg2, site);
    if (core->flight->wrapped()) {
        static_cast<void>(core->status().flags.fetch_or<
            libk::MemoryOrder::Release>(DiagnosticStatus::FlightWrapped));
    }
    if (core->flight->degraded()) {
        static_cast<void>(core->status().flags.fetch_or<
            libk::MemoryOrder::Release>(DiagnosticStatus::FlightGap));
    }
#else
    static_cast<void>(domain);
    static_cast<void>(event);
    static_cast<void>(actor);
    static_cast<void>(subject);
    static_cast<void>(arg0);
    static_cast<void>(arg1);
    static_cast<void>(arg2);
    static_cast<void>(site);
#endif
}

void dispatch(CpuId cpu, u64 actor, u64 context, u64 tick) noexcept {
#if MYOS_CONCURRENCY_DIAG >= 1
    if (CpuDiagnosticsCore* const core = current_core(); core != nullptr) {
        core->live.current_actor.store<libk::MemoryOrder::Release>(actor);
        static_cast<void>(core->live.dispatch_epoch.fetch_add<libk::MemoryOrder::Release>(1));
        core->live.last_event_at.store<libk::MemoryOrder::Release>(tick);
    }
#endif
    static_cast<void>(cpu);
    static_cast<void>(actor);
    static_cast<void>(context);
    static_cast<void>(tick);
}


void timer(CpuId cpu, u64 tick) noexcept {
#if MYOS_CONCURRENCY_DIAG >= 1
    if (CpuDiagnosticsCore* const core = current_core(); core != nullptr) {
        static_cast<void>(core->live.timer_epoch.fetch_add<libk::MemoryOrder::Release>(1));
        core->live.last_event_at.store<libk::MemoryOrder::Release>(tick);
    }
#endif
    record(FlightDomain::Cpu, FlightEvent::Timer, 0, 0, cpu.raw, tick);
}

auto CpuLive::top_wait(WaitSnapshot& result) const noexcept -> bool {
    const u32 depth = wait_depth.load<libk::MemoryOrder::Acquire>();
    if (depth == 0 || depth > wait_capacity) {
        return false;
    }
    const WaitFrame& frame = waits[depth - 1];
    for (usize attempt = 0; attempt < 3; ++attempt) {
        const u64 first = AtomicSnapshotReader::begin(frame.sequence);
        if ((first & 1U) != 0) {
            continue;
        }
        const u64 token = frame.token.load<libk::MemoryOrder::Relaxed>();
        if (token == 0) {
            continue;
        }
        WaitSnapshot value{};
        value.wait = frame.wait.load<libk::MemoryOrder::Relaxed>();
        const u64 kinds = frame.kinds.load<libk::MemoryOrder::Relaxed>();
        value.subject = read_node(
            frame.subject_identity.load<libk::MemoryOrder::Relaxed>(),
            static_cast<u32>(kinds & 0xffU),
            frame.subject_generation.load<libk::MemoryOrder::Relaxed>());
        value.driver = read_node(
            frame.driver_identity.load<libk::MemoryOrder::Relaxed>(),
            static_cast<u32>((kinds >> 8) & 0xffU),
            frame.driver_generation.load<libk::MemoryOrder::Relaxed>());
        value.obligation = frame.obligation.load<libk::MemoryOrder::Relaxed>();
        value.since = frame.since.load<libk::MemoryOrder::Relaxed>();
        value.kind = static_cast<WaitKind>((kinds >> 16) & 0xffU);
        value.site.file = reinterpret_cast<const char*>(
            frame.site_file.load<libk::MemoryOrder::Relaxed>());
        value.site.line = frame.site_line.load<libk::MemoryOrder::Relaxed>();
        if (AtomicSnapshotReader::valid(frame.sequence, first)
            && frame.token.load<libk::MemoryOrder::Acquire>() == token
            && wait_depth.load<libk::MemoryOrder::Acquire>() == depth) {
            result = value;
            return true;
        }
    }
    return false;
}

auto CpuLive::snapshot(
    Snapshot& result,
    SnapshotMode mode) const noexcept -> bool {
    const auto read_scalars = [&](Snapshot& value) noexcept -> u32 {
        value.dispatch_epoch =
            dispatch_epoch.load<libk::MemoryOrder::Acquire>();
        value.timer_epoch = timer_epoch.load<libk::MemoryOrder::Acquire>();
        value.trap_entered_at =
            trap_entered_at.load<libk::MemoryOrder::Acquire>();
        value.irq_disabled_since =
            irq_disabled_since.load<libk::MemoryOrder::Acquire>();
        value.current_actor =
            current_actor.load<libk::MemoryOrder::Acquire>();
        value.activity_epoch =
            wait_activity_epoch.load<libk::MemoryOrder::Acquire>();
        value.progress_epoch =
            wait_progress_epoch.load<libk::MemoryOrder::Acquire>();
        value.semantic_stamp =
            wait_semantic_stamp.load<libk::MemoryOrder::Acquire>();
        value.last_event_at =
            last_event_at.load<libk::MemoryOrder::Acquire>();
        value.context = context.load<libk::MemoryOrder::Acquire>();
        value.trap_depth = trap_depth.load<libk::MemoryOrder::Acquire>();
        value.irq_depth = irq_depth.load<libk::MemoryOrder::Acquire>();
        value.degraded = degraded.load<libk::MemoryOrder::Acquire>();
        if (wait_overflow.load<libk::MemoryOrder::Acquire>() != 0) {
            value.degraded |= DiagnosticStatus::WaitStackOverflow;
        }
        value.interrupts_disabled =
            interrupts_disabled.load<libk::MemoryOrder::Acquire>();
        return wait_depth.load<libk::MemoryOrder::Acquire>();
    };
    const auto same_scalars = [mode](const Snapshot& left,
                                     const Snapshot& right) noexcept {
        return left.dispatch_epoch == right.dispatch_epoch
            && left.trap_entered_at == right.trap_entered_at
            && left.irq_disabled_since == right.irq_disabled_since
            && left.current_actor == right.current_actor
            && left.progress_epoch == right.progress_epoch
            && left.semantic_stamp == right.semantic_stamp
            && left.context == right.context
            && left.trap_depth == right.trap_depth
            && left.irq_depth == right.irq_depth
            && left.degraded == right.degraded
            && left.interrupts_disabled == right.interrupts_disabled
            && (mode == SnapshotMode::Relation
                || (left.timer_epoch == right.timer_epoch
                    && left.activity_epoch == right.activity_epoch
                    && left.last_event_at == right.last_event_at));
    };
    const auto same_wait = [](const WaitSnapshot& left,
                              const WaitSnapshot& right) noexcept {
        return left.wait == right.wait
            && left.subject == right.subject
            && left.driver == right.driver
            && left.obligation == right.obligation
            && left.since == right.since
            && left.kind == right.kind
            && left.site.file == right.site.file
            && left.site.line == right.site.line;
    };
    for (usize attempt = 0; attempt < 3; ++attempt) {
        Snapshot first{};
        const u32 depth = read_scalars(first);
        if (depth > wait_capacity) {
            continue;
        }
        first.has_wait = depth != 0;
        if (first.has_wait && !top_wait(first.wait)) {
            continue;
        }
        Snapshot second{};
        const u32 second_depth = read_scalars(second);
        second.has_wait = second_depth != 0;
        if (second.has_wait && !top_wait(second.wait)) {
            continue;
        }
        if (depth != second_depth
            || !same_scalars(first, second)
            || (first.has_wait && !same_wait(first.wait, second.wait))) {
            continue;
        }
        result = first;
        return true;
    }
    return false;
}

void watchdog_tick(CpuId cpu, u64 tick) noexcept {
#if MYOS_CONCURRENCY_DIAG >= 3
    CpuDiagnosticsCore* const watcher = current_core();
    if (watcher == nullptr) {
        return;
    }
    // The mailbox is the only durable report-pending truth.  A failed
    // observer wake (Busy, Unavailable, Retained, or notifier contention) is
    // retried from the next watchdog tick without a second pending registry;
    // the attempt remains one bounded notify and the RemoteRequest coalesces
    // any already-retained wake edge.
    if (watcher->reports.pending()) {
        static_cast<void>(report_notifier.notify());
    }
    // A fixed ring assignment gives every CPU one peer to inspect while
    // keeping the candidate single-writer. If a peer is unavailable, inspect
    // the local shard; this also covers single-hart systems.
    CpuDiagnosticsCore* target_core = watcher;
    ObservationShard* shard = current_shard();
    CpuId target_cpu = cpu;
    void* const owner = arch::current_cpu_owner();
    if (owner != nullptr) {
        auto& local = *static_cast<CpuLocal*>(owner);
        CpuRegistry* const registry = local.runtime_ == nullptr
            ? nullptr : local.runtime_->owner_registry;
        if (registry != nullptr && registry->count() > 1) {
            CpuId target{(cpu.raw + 1) % registry->count()};
            for (usize attempt = 0; attempt < registry->count(); ++attempt) {
                const CpuDescriptor* const descriptor =
                    registry->descriptor(target);
                const CpuRuntime* const runtime = registry->runtime(target);
                if (descriptor != nullptr && runtime != nullptr
                    && descriptor->state() == CpuState::Online
                    && runtime->diagnostics != nullptr
                    && runtime->diagnostics->concurrency->observations != nullptr) {
                    target_core = runtime->diagnostics->concurrency;
                    shard = target_core->observations;
                    target_cpu = target;
                    break;
                }
                target = CpuId{(target.raw + 1) % registry->count()};
            }
        }
    }
    u64 touched_candidates{};
    const auto process = [&](StallFingerprint sample,
                             WatchdogThresholds thresholds,
                             StallAction action,
                             WaitKind wait_kind,
                             const ObservationSnapshot* root_snapshot = nullptr)
        noexcept -> bool {
        WatchdogCandidate* slot{};
        usize slot_index{};
        for (usize index = 0;
             index < CpuDiagnosticsCore::candidate_capacity;
             ++index) {
            WatchdogCandidate& candidate = watcher->candidates[index];
            StallFingerprint existing{};
            if (candidate.read(existing) && existing.root == sample.root) {
                slot = &candidate;
                slot_index = index;
                break;
            }
        }
        if (slot != nullptr && (touched_candidates & bit(slot_index)) != 0) {
            return false;
        }
        if (slot == nullptr) {
            const usize start = watcher->candidate_cursor.fetch_add<
                libk::MemoryOrder::AcqRel>(1);
            for (usize offset = 0;
                 offset < CpuDiagnosticsCore::candidate_capacity;
                 ++offset) {
                const usize candidate =
                    (start + offset) % CpuDiagnosticsCore::candidate_capacity;
                if ((touched_candidates & bit(candidate)) == 0) {
                    slot_index = candidate;
                    slot = &watcher->candidates[candidate];
                    break;
                }
            }
            if (slot == nullptr) {
                return false;
            }
            slot->state.store<libk::MemoryOrder::Release>(
                static_cast<u32>(WatchdogCandidate::State::Clear));
            slot->active_intervals.store<libk::MemoryOrder::Release>(0);
        }
        touched_candidates |= bit(slot_index);

        StallFingerprint previous{};
        const bool valid = slot->read(previous);
        auto state = static_cast<WatchdogCandidate::State>(
            slot->state.load<libk::MemoryOrder::Acquire>());
        const bool same = valid
            && previous.root == sample.root
            && previous.phase == sample.phase
            && previous.progress_epoch == sample.progress_epoch
            && previous.relation_hash == sample.relation_hash
            && previous.driver == sample.driver
            && previous.blocker == sample.blocker;
        if (!same) {
            slot->publish(sample);
            slot->first_seen.store<libk::MemoryOrder::Release>(0);
            slot->active_intervals.store<libk::MemoryOrder::Release>(0);
            slot->state.store<libk::MemoryOrder::Release>(
                static_cast<u32>(WatchdogCandidate::State::Clear));
            return true;
        }

        const bool active =
            sample.activity_epoch != previous.activity_epoch;
        slot->publish(sample);
        if (tick < thresholds.soft_at) {
            return true;
        }
        u32 active_intervals = slot->active_intervals.load<
            libk::MemoryOrder::Relaxed>();
        if (active) {
            if (active_intervals != libk::numeric_limits<u32>::max()) {
                ++active_intervals;
            }
        } else {
            active_intervals = 0;
        }
        slot->active_intervals.store<libk::MemoryOrder::Release>(
            active_intervals);
        const u64 age = tick >= thresholds.anchor
            ? elapsed(tick, thresholds.anchor) : 0;
        if (state == WatchdogCandidate::State::Clear) {
            slot->first_seen.store<libk::MemoryOrder::Release>(tick);
            state = WatchdogCandidate::State::Suspected;
            slot->state.store<libk::MemoryOrder::Release>(
                static_cast<u32>(state));
            record(
                FlightDomain::Watchdog,
                FlightEvent::WatchdogSuspected,
                cpu.raw,
                sample.root.identity,
                static_cast<u64>(wait_kind),
                age);
            return true;
        }
        if (state != WatchdogCandidate::State::Suspected
            || tick < thresholds.hard_at) {
            return true;
        }
        const bool livelock = active_intervals >= 2;

        u64 report_signature{};
        bool owns_report{};
        if (action != StallAction::Record) {
            report_signature = mix_hash(
                mix_hash(
                    mix_hash(
                        mix_hash(
                            mix_hash(u64{0x27d4eb2f165667c5},
                                static_cast<u64>(sample.root.kind)),
                            sample.root.identity),
                        sample.root.generation),
                    sample.progress_epoch),
                sample.relation_hash);
            report_signature = mix_hash(
                report_signature, thresholds.anchor);
            if (report_signature == 0) {
                report_signature = 1;
            }
            u64 expected{};
            const u64 owner_token = cpu.raw + 1;
            if (!target_core->coordinator.owner.compare_exchange_strong<
                    libk::MemoryOrder::AcqRel,
                    libk::MemoryOrder::Acquire>(
                        expected, owner_token)) {
                return true;
            }
            owns_report = true;
            bool duplicate_report{};
            for (const libk::Atomic<u64>& signature
                 : target_core->coordinator.signatures) {
                duplicate_report = duplicate_report
                    || signature.load<libk::MemoryOrder::Acquire>()
                        == report_signature;
            }
            if (duplicate_report) {
                const auto duplicate = livelock
                    ? WatchdogCandidate::State::ConfirmedLivelock
                    : WatchdogCandidate::State::Confirmed;
                slot->state.store<libk::MemoryOrder::Release>(
                    static_cast<u32>(duplicate));
                target_core->coordinator.owner.store<
                    libk::MemoryOrder::Release>(0);
                return true;
            }
        }
        const auto release_report = [&]() noexcept {
            if (owns_report) {
                target_core->coordinator.owner.store<
                    libk::MemoryOrder::Release>(0);
            }
        };

        WaitGraphScratch& graph = watcher->graph;
        if (!analyze(sample.root, graph)
            || graph.evidence != EvidenceGrade::Confirmed) {
            record(
                FlightDomain::Watchdog,
                FlightEvent::ObservationDegraded,
                cpu.raw,
                sample.root.identity,
                static_cast<u64>(graph.evidence),
                static_cast<u64>(graph.classification));
            release_report();
            return true;
        }
        const auto confirmed = livelock
            ? WatchdogCandidate::State::ConfirmedLivelock
            : WatchdogCandidate::State::Confirmed;
        if (livelock) {
            graph.classification = StallClass::Livelock;
        }
        slot->state.store<libk::MemoryOrder::Release>(
            static_cast<u32>(confirmed));
        record(
            FlightDomain::Watchdog,
            livelock ? FlightEvent::WatchdogLivelock
                     : FlightEvent::WatchdogConfirmed,
            cpu.raw,
            sample.root.identity,
            static_cast<u64>(confirmed),
            age);
        if (owns_report) {
            const usize signature_index =
                target_core->coordinator.signature_cursor.fetch_add<
                    libk::MemoryOrder::AcqRel>(1)
                % StallCoordinator::signature_capacity;
            target_core->coordinator.signatures[signature_index].store<
                libk::MemoryOrder::Release>(report_signature);
        }
        if (action == StallAction::Report) {
            if (queue_report(
                    cpu,
                    target_cpu,
                    sample.root,
                    confirmed,
                    graph.classification,
                    graph.evidence,
                    age,
                    root_snapshot,
                    graph)) {
                static_cast<void>(target_core->status().flags.fetch_or<
                    libk::MemoryOrder::Release>(
                    DiagnosticStatus::StallReported));
            }
        }
        release_report();
        return true;
    };

    // A local timer callback cannot prove that its own interrupts or trap
    // return are wedged. CPU-live roots therefore require an independent peer.
    if (target_core != watcher) {
        CpuLive::Snapshot live{};
        if (target_core->live.snapshot(live)) {
            const auto inspect_live = [&](u64 entered,
                                          DiagnosticStatus::Flag flag,
                                          u32 phase) noexcept {
                if (entered == 0
                    || target_core->policy.critical_soft == 0
                    || target_core->policy.critical_hard == 0) {
                    static_cast<void>(target_core->status().flags.fetch_and<
                        libk::MemoryOrder::AcqRel>(
                            ~static_cast<u32>(flag)));
                    return;
                }
                const u64 soft_at = add_sat(
                    entered, target_core->policy.critical_soft);
                if (tick >= soft_at) {
                    static_cast<void>(target_core->status().flags.fetch_or<
                        libk::MemoryOrder::Release>(flag));
                } else {
                    static_cast<void>(target_core->status().flags.fetch_and<
                        libk::MemoryOrder::AcqRel>(
                            ~static_cast<u32>(flag)));
                }
                u64 relation = mix_hash(
                    u64{0x7e92d8a153}, target_cpu.raw);
                relation = mix_hash(relation, phase);
                relation = mix_hash(relation, entered);
                relation = mix_hash(relation, live.current_actor);
                relation = mix_hash(relation, live.context);
                process(
                    StallFingerprint{
                        NodeRef{
                            NodeRef::Kind::Cpu,
                            target_cpu.raw,
                            cpu_stall_generation(entered, phase)},
                        phase,
                        0,
                        live.last_event_at,
                        relation,
                        live.has_wait ? live.wait.driver : NodeRef{},
                        {}},
                    WatchdogThresholds{
                        entered,
                        soft_at,
                        add_sat(
                            entered,
                            target_core->policy.critical_hard)},
                    StallAction::Report,
                    phase == 1 ? WaitKind::Irq : WaitKind::Unknown);
            };
            inspect_live(
                live.irq_disabled_since, DiagnosticStatus::IrqStall, 1);
            inspect_live(
                live.trap_entered_at, DiagnosticStatus::TrapStall, 2);
        }
    }

    if (shard == nullptr) {
        return;
    }
    const u64 watched = shard->watched();
    const usize start = watcher->scan_cursor.fetch_add<
        libk::MemoryOrder::AcqRel>(1)
        % ObservationShard::slot_count;
    usize accepted{};
    for (usize offset = 0;
         offset < ObservationShard::slot_count
             && accepted < CpuDiagnosticsCore::candidate_capacity;
         ++offset) {
        const usize index =
            (start + offset) % ObservationShard::slot_count;
        if ((watched & bit(index)) == 0) {
            continue;
        }
        const ObservationKey key = shard->key_at(index);
        ObservationSnapshot snapshot{};
        if (!key || !shard->snapshot(key, snapshot)) {
            continue;
        }
        ObservationSnapshot wait_target{};
        const ObservationSnapshot* delegated{};
        if (snapshot.record_kind == RecordKind::ExecutionActor
            && snapshot.phase == static_cast<u32>(
                ExecutionState::Blocked)) {
            if (!snapshot.wait_target) {
                static_cast<void>(target_core->status().flags.fetch_or<
                    libk::MemoryOrder::Release>(
                        DiagnosticStatus::PolicyMissing));
                continue;
            }
            ObservationLease lease =
                ObservationLease::borrow(snapshot.wait_target);
            if (!lease || !lease.snapshot(wait_target)) {
                static_cast<void>(target_core->status().flags.fetch_or<
                    libk::MemoryOrder::Release>(
                        DiagnosticStatus::PolicyMissing));
                continue;
            }
            delegated = &wait_target;
        }
        const WatchdogThresholds thresholds = thresholds_for(
            target_core->policy, snapshot, delegated);
        if (!thresholds) {
            continue;
        }
        u64 relation = snapshot_hash(snapshot, HashMode::Relation);
        u64 progress = snapshot.progress_epoch;
        u64 activity = snapshot.activity_epoch;
        if (delegated != nullptr) {
            relation = mix_hash(
                relation,
                snapshot_hash(*delegated, HashMode::Relation));
            progress = mix_hash(progress, delegated->progress_epoch);
            activity = mix_hash(activity, delegated->activity_epoch);
        }
        if (process(
            StallFingerprint{
                NodeRef::observation(key),
                snapshot.phase,
                progress,
                activity,
                relation,
                snapshot.driver,
                snapshot.blocker},
            thresholds,
            snapshot.policy.action,
            snapshot.wait_kind,
            &snapshot)) {
            ++accepted;
        }
    }
#else
    static_cast<void>(cpu);
    static_cast<void>(tick);
#endif
}

void trap_enter(u64 tick, u32 context) noexcept {
#if MYOS_CONCURRENCY_DIAG >= 1
    if (CpuDiagnosticsCore* const core = current_core(); core != nullptr) {
        const u32 depth = core->live.trap_depth.fetch_add<
            libk::MemoryOrder::AcqRel>(1);
        if (depth == 0) {
            core->live.trap_entered_at.store<libk::MemoryOrder::Release>(tick);
        }
        core->live.context.store<libk::MemoryOrder::Release>(context);
    }
#endif
    record(FlightDomain::Trap, FlightEvent::TrapEnter, 0, 0, context, tick);
}

void trap_exit(u64 tick) noexcept {
#if MYOS_CONCURRENCY_DIAG >= 1
    if (CpuDiagnosticsCore* const core = current_core(); core != nullptr) {
        const u32 depth = core->live.trap_depth.load<
            libk::MemoryOrder::Acquire>();
        if (depth == 0) {
            mark_degraded(DiagnosticStatus::SnapshotUnstable);
        } else if (core->live.trap_depth.fetch_sub<
                       libk::MemoryOrder::AcqRel>(1) == 1) {
            core->live.trap_entered_at.store<libk::MemoryOrder::Release>(0);
        }
        core->live.last_event_at.store<libk::MemoryOrder::Release>(tick);
    }
#endif
    record(FlightDomain::Trap, FlightEvent::TrapExit, 0, 0, tick);
}

auto irq_disabled(SourceSite site) noexcept -> u64 {
#if MYOS_CONCURRENCY_DIAG >= 1
    CpuDiagnosticsCore* const core = current_core();
    if (core == nullptr) {
        return 0;
    }
    const u64 tick = now();
    const u32 depth = core->live.irq_depth.fetch_add<
        libk::MemoryOrder::AcqRel>(1);
    if (depth == 0) {
        core->live.irq_disabled_since.store<libk::MemoryOrder::Release>(tick);
    }
    core->live.interrupts_disabled.store<libk::MemoryOrder::Release>(true);
    if (depth == 0) {
        record(FlightDomain::Irq, FlightEvent::IrqOff, 0, 0, tick, 0, 0, site);
    }
    // The cookie is only an ownership marker; the timestamp lives in live.
    // Keep it non-zero even when the clock is still at tick zero.
    return 1;
#else
    static_cast<void>(site);
    return 0;
#endif
}

void irq_restoring(u64 cookie) noexcept {
#if MYOS_CONCURRENCY_DIAG >= 1
    CpuDiagnosticsCore* const core = current_core();
    if (core == nullptr || cookie == 0) {
        return;
    }
    const u32 depth = core->live.irq_depth.load<libk::MemoryOrder::Acquire>();
    if (depth == 0) {
        mark_degraded(DiagnosticStatus::SnapshotUnstable);
        return;
    }
    if (core->live.irq_depth.fetch_sub<libk::MemoryOrder::AcqRel>(1) == 1) {
        const u64 previous = core->live.irq_disabled_since.exchange<
            libk::MemoryOrder::AcqRel>(0);
        core->live.interrupts_disabled.store<libk::MemoryOrder::Release>(false);
        record(FlightDomain::Irq, FlightEvent::IrqOn, 0, 0, now(), previous);
    }
#else
    static_cast<void>(cookie);
#endif
}

auto default_grace(Expectation expectation) noexcept -> u64 {
#if MYOS_CONCURRENCY_DIAG >= 1
    const CpuDiagnosticsCore* const core = current_core();
    if (core == nullptr) {
        return 0;
    }
    return expectation == Expectation::SchedulerControlled
        ? core->policy.scheduler_hard : core->policy.critical_hard;
#else
    static_cast<void>(expectation);
    return 0;
#endif
}

auto set_wait(
    WaitKind kind,
    ObservationKey wait,
    NodeRef subject,
    NodeRef driver,
    SourceSite site) noexcept -> WaitToken {
    WaitToken token{};
#if MYOS_CONCURRENCY_DIAG >= 1
    if (CpuDiagnosticsCore* const core = current_core(); core != nullptr) {
        const u64 tick = now();
        u64 generation = core->live.wait_generation.load<
            libk::MemoryOrder::Relaxed>();
        while (generation < wait_token_generation_max
            && !core->live.wait_generation.compare_exchange_weak<
                libk::MemoryOrder::AcqRel,
                libk::MemoryOrder::Relaxed>(
                    generation, generation + 1)) {}
        const bool generation_ready =
            generation < wait_token_generation_max;
        if (!generation_ready) {
            static_cast<void>(core->status().flags.fetch_or<
                libk::MemoryOrder::Release>(
                    DiagnosticStatus::ObservationGenerationExhausted));
        } else {
            ++generation;
        }
        u32 depth = core->live.wait_depth.load<libk::MemoryOrder::Relaxed>();
        if (generation_ready && depth >= CpuLive::wait_capacity) {
            u32 overflow = core->live.wait_overflow.load<
                libk::MemoryOrder::Relaxed>();
            while (overflow != libk::numeric_limits<u32>::max()
                && !core->live.wait_overflow.compare_exchange_weak<
                    libk::MemoryOrder::AcqRel,
                    libk::MemoryOrder::Relaxed>(overflow, overflow + 1)) {}
            static_cast<void>(core->live.degraded.fetch_or<
                libk::MemoryOrder::Release>(DiagnosticStatus::WaitStackOverflow));
            static_cast<void>(core->status().flags.fetch_or<
                libk::MemoryOrder::Release>(DiagnosticStatus::WaitStackOverflow));
            if (overflow != libk::numeric_limits<u32>::max()) {
                token = make_wait_token(generation, wait_token_overflow);
            }
        } else if (generation_ready) {
            CpuLive::WaitFrame& frame = core->live.waits[depth];
            u64 odd{};
            if (AtomicSnapshotWriter::begin(frame.sequence, odd)) {
                token = make_wait_token(generation, depth + 1);
                frame.token.store<libk::MemoryOrder::Relaxed>(token.raw);
                frame.wait.store<libk::MemoryOrder::Relaxed>(wait.raw);
                frame.subject_identity.store<libk::MemoryOrder::Relaxed>(
                    subject.identity);
                frame.subject_generation.store<libk::MemoryOrder::Relaxed>(
                    subject.generation);
                frame.driver_identity.store<libk::MemoryOrder::Relaxed>(
                    driver.identity);
                frame.driver_generation.store<libk::MemoryOrder::Relaxed>(
                    driver.generation);
                frame.kinds.store<libk::MemoryOrder::Relaxed>(
                    static_cast<u64>(static_cast<u8>(subject.kind))
                    | (static_cast<u64>(static_cast<u8>(driver.kind)) << 8)
                    | (static_cast<u64>(static_cast<u8>(kind)) << 16));
                frame.obligation.store<libk::MemoryOrder::Relaxed>(
                    wait ? wait.raw : subject.identity);
                frame.since.store<libk::MemoryOrder::Relaxed>(tick);
                frame.site_file.store<libk::MemoryOrder::Relaxed>(
                    reinterpret_cast<usize>(site.file));
                frame.site_line.store<libk::MemoryOrder::Relaxed>(site.line);
                AtomicSnapshotWriter::end(frame.sequence, odd);
                core->live.wait_activity_epoch.store<
                    libk::MemoryOrder::Release>(0);
                core->live.wait_progress_epoch.store<
                    libk::MemoryOrder::Release>(0);
                core->live.wait_semantic_stamp.store<
                    libk::MemoryOrder::Release>(0);
                core->live.wait_depth.store<libk::MemoryOrder::Release>(
                    depth + 1);
            } else {
                static_cast<void>(core->status().flags.fetch_or<
                    libk::MemoryOrder::Release>(
                    DiagnosticStatus::ObservationWriterCollision));
            }
        }
    }
#endif
    const bool lock_wait = kind == WaitKind::SpinLock;
    record(
        lock_wait ? FlightDomain::Lock : FlightDomain::Operation,
        lock_wait ? FlightEvent::LockContended : FlightEvent::OperationAttach,
        wait.raw,
        subject.identity,
        static_cast<u64>(kind),
        driver.identity,
        0,
        site);
    return token;
}

void clear_wait(WaitToken token) noexcept {
#if MYOS_CONCURRENCY_DIAG >= 1
    if (!token) {
        return;
    }
    if (CpuDiagnosticsCore* const core = current_core(); core != nullptr) {
        const u64 depth_code = wait_token_depth(token);
        if (depth_code == wait_token_overflow) {
            u32 overflow = core->live.wait_overflow.load<
                libk::MemoryOrder::Acquire>();
            while (overflow != 0
                && !core->live.wait_overflow.compare_exchange_weak<
                    libk::MemoryOrder::AcqRel,
                    libk::MemoryOrder::Acquire>(overflow, overflow - 1)) {}
            if (overflow == 0) {
                static_cast<void>(core->status().flags.fetch_or<
                    libk::MemoryOrder::Release>(
                        DiagnosticStatus::SnapshotUnstable));
            }
            return;
        }
        const u32 depth = core->live.wait_depth.load<
            libk::MemoryOrder::Acquire>();
        if (depth_code == 0 || depth_code > CpuLive::wait_capacity
            || depth != depth_code) {
            static_cast<void>(core->status().flags.fetch_or<
                libk::MemoryOrder::Release>(DiagnosticStatus::SnapshotUnstable));
        } else {
            CpuLive::WaitFrame& frame = core->live.waits[depth - 1];
            if (frame.token.load<libk::MemoryOrder::Acquire>() != token.raw) {
                static_cast<void>(core->status().flags.fetch_or<
                    libk::MemoryOrder::Release>(
                        DiagnosticStatus::SnapshotUnstable));
                return;
            }
            u64 odd{};
            if (AtomicSnapshotWriter::begin(frame.sequence, odd)) {
                if (frame.token.load<libk::MemoryOrder::Relaxed>()
                        != token.raw
                    || core->live.wait_depth.load<
                        libk::MemoryOrder::Relaxed>() != depth) {
                    AtomicSnapshotWriter::end(frame.sequence, odd);
                    static_cast<void>(core->status().flags.fetch_or<
                        libk::MemoryOrder::Release>(
                            DiagnosticStatus::SnapshotUnstable));
                    return;
                }
                frame.token.store<libk::MemoryOrder::Relaxed>(0);
                frame.wait.store<libk::MemoryOrder::Relaxed>(0);
                frame.subject_identity.store<libk::MemoryOrder::Relaxed>(0);
                frame.subject_generation.store<libk::MemoryOrder::Relaxed>(0);
                frame.driver_identity.store<libk::MemoryOrder::Relaxed>(0);
                frame.driver_generation.store<libk::MemoryOrder::Relaxed>(0);
                frame.obligation.store<libk::MemoryOrder::Relaxed>(0);
                frame.since.store<libk::MemoryOrder::Relaxed>(0);
                frame.site_file.store<libk::MemoryOrder::Relaxed>(0);
                frame.site_line.store<libk::MemoryOrder::Relaxed>(0);
                frame.kinds.store<libk::MemoryOrder::Relaxed>(
                    static_cast<u64>(static_cast<u8>(WaitKind::None)) << 16);
                AtomicSnapshotWriter::end(frame.sequence, odd);
                core->live.wait_depth.store<libk::MemoryOrder::Release>(
                    depth - 1);
            }
        }
    }
#else
    static_cast<void>(token);
#endif
}

auto retarget_wait(
    WaitToken token,
    NodeRef driver,
    SourceSite site) noexcept -> bool {
#if MYOS_CONCURRENCY_DIAG >= 1
    CpuDiagnosticsCore* const core = current_core();
    const u64 depth_code = wait_token_depth(token);
    if (core == nullptr || !token || depth_code == 0
        || depth_code == wait_token_overflow
        || depth_code > CpuLive::wait_capacity
        || core->live.wait_depth.load<libk::MemoryOrder::Acquire>()
            != depth_code) {
        return false;
    }
    CpuLive::WaitFrame& frame = core->live.waits[depth_code - 1];
    if (frame.token.load<libk::MemoryOrder::Acquire>() != token.raw) {
        return false;
    }
    u64 odd{};
    if (!AtomicSnapshotWriter::begin(frame.sequence, odd)) {
        static_cast<void>(core->status().flags.fetch_or<
            libk::MemoryOrder::Release>(
                DiagnosticStatus::ObservationWriterCollision));
        return false;
    }
    if (frame.token.load<libk::MemoryOrder::Relaxed>() != token.raw
        || core->live.wait_depth.load<libk::MemoryOrder::Relaxed>()
            != depth_code) {
        AtomicSnapshotWriter::end(frame.sequence, odd);
        return false;
    }
    frame.driver_identity.store<libk::MemoryOrder::Relaxed>(driver.identity);
    frame.driver_generation.store<libk::MemoryOrder::Relaxed>(
        driver.generation);
    u64 kinds = frame.kinds.load<libk::MemoryOrder::Relaxed>();
    kinds = (kinds & ~(u64{0xff} << 8))
        | (static_cast<u64>(static_cast<u8>(driver.kind)) << 8);
    frame.kinds.store<libk::MemoryOrder::Relaxed>(kinds);
    frame.site_file.store<libk::MemoryOrder::Relaxed>(
        reinterpret_cast<usize>(site.file));
    frame.site_line.store<libk::MemoryOrder::Relaxed>(site.line);
    AtomicSnapshotWriter::end(frame.sequence, odd);
    return true;
#else
    static_cast<void>(token);
    static_cast<void>(driver);
    static_cast<void>(site);
    return false;
#endif
}

void observe_wait(WaitToken token, u64 semantic_stamp) noexcept {
#if MYOS_CONCURRENCY_DIAG >= 1
    CpuDiagnosticsCore* const core = current_core();
    const u64 depth_code = wait_token_depth(token);
    if (core == nullptr || !token || depth_code == 0
        || depth_code == wait_token_overflow
        || depth_code > CpuLive::wait_capacity
        || core->live.wait_depth.load<libk::MemoryOrder::Acquire>()
            != depth_code
        || core->live.waits[depth_code - 1].token.load<
            libk::MemoryOrder::Acquire>() != token.raw) {
        return;
    }
    static_cast<void>(core->live.wait_activity_epoch.fetch_add<
        libk::MemoryOrder::Relaxed>(1));
    const u64 previous = core->live.wait_semantic_stamp.load<
        libk::MemoryOrder::Acquire>();
    if (previous != semantic_stamp) {
        core->live.wait_semantic_stamp.store<libk::MemoryOrder::Release>(
            semantic_stamp);
        static_cast<void>(core->live.wait_progress_epoch.fetch_add<
            libk::MemoryOrder::Relaxed>(1));
    }
#else
    static_cast<void>(token);
    static_cast<void>(semantic_stamp);
#endif
}

void mark_degraded(u32 flag) noexcept {
#if MYOS_CONCURRENCY_DIAG >= 1
    if (CpuDiagnosticsCore* const core = current_core(); core != nullptr) {
        static_cast<void>(core->status().flags.fetch_or<libk::MemoryOrder::Release>(flag));
        static_cast<void>(core->live.degraded.fetch_or<libk::MemoryOrder::Release>(flag));
    }
#else
    static_cast<void>(flag);
#endif
}


void dump_flight(CpuId id, const FlightRecorder& flight) noexcept {
    if (!enabled(Level::Trace)) {
        return;
    }
    diag::console::print<"[concurrency] cpu {} flight\n">(id.raw);
    FlightRecordValue record_value{};
    const u64 head = flight.head();
    const usize count = head < FlightRecorder::capacity
        ? static_cast<usize>(head) : FlightRecorder::capacity;
    for (usize index = 0; index < count; ++index) {
        if (!flight.read(index, record_value)) {
            continue;
        }
        diag::console::print<
            "  index={} sequence={} absolute={} tick={} domain={} event={} "
            "actor={:#x} subject={:#x} a0={:#x} a1={:#x} a2={:#x} "
            "site={}:{} function={}\n">(
            index,
            record_value.sequence,
            record_value.absolute_id,
            record_value.tick,
            static_cast<u32>(record_value.domain),
            static_cast<u32>(record_value.event),
            record_value.actor,
            record_value.subject,
            record_value.arg0,
            record_value.arg1,
            record_value.arg2,
            record_value.site.file,
            record_value.site.line,
            record_value.site.function);
    }
}

#if MYOS_CONCURRENCY_DIAG >= 1
namespace {

[[nodiscard]] auto is_external_wait(WaitKind kind) noexcept -> bool {
    switch (kind) {
    case WaitKind::Notification:
    case WaitKind::EndpointReply:
    case WaitKind::ChannelReceive:
    case WaitKind::Pager:
    case WaitKind::External:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] auto is_transport_wait(WaitKind kind) noexcept -> bool {
    switch (kind) {
    case WaitKind::CompletionDelivery:
    case WaitKind::RemoteRequest:
    case WaitKind::IpiDelivery:
    case WaitKind::ShootdownAck:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] auto is_drain_wait(WaitKind kind) noexcept -> bool {
    switch (kind) {
    case WaitKind::GrantWork:
    case WaitKind::GrantOperations:
    case WaitKind::GrantAttachments:
    case WaitKind::ResourceReservations:
    case WaitKind::ResourceConstructions:
    case WaitKind::ResourceChildren:
    case WaitKind::ResourceRefund:
    case WaitKind::ObjectCleanup:
    case WaitKind::ObjectReferences:
    case WaitKind::ObjectPins:
    case WaitKind::ObjectReclaim:
    case WaitKind::VSpaceClaim:
    case WaitKind::VSpaceShootdown:
    case WaitKind::VSpaceAuthorityDrain:
    case WaitKind::VSpaceActiveCpus:
    case WaitKind::VSpaceWork:
        return true;
    default:
        return false;
    }
}

// These values are the canonical ExecutionState and Completion delivery
// values.  They are kept local to the analyzer: diagnostics interprets the
// published projection but never becomes a second state owner.
constexpr u32 execution_ready_phase = static_cast<u32>(
    ExecutionState::Ready);
constexpr u32 execution_throttled_phase = static_cast<u32>(
    ExecutionState::Throttled);
constexpr u32 execution_blocked_phase = static_cast<u32>(
    ExecutionState::Blocked);

[[nodiscard]] auto terminal_class(
    const ObservationSnapshot& snapshot,
    bool livelock) noexcept -> StallClass {
    if (livelock) {
        return StallClass::Livelock;
    }
    if (snapshot.expectation == Expectation::DeadlineBound) {
        return StallClass::DeadlineDeliveryStall;
    }
    if (snapshot.expectation == Expectation::ExternalUnbounded
        || is_external_wait(snapshot.wait_kind)) {
        return StallClass::ExternalWait;
    }
    const bool service_unlinked =
        snapshot.driver.kind == NodeRef::Kind::None
        && (
            (snapshot.record_kind == RecordKind::ServiceWork
                && snapshot.phase == static_cast<u32>(
                    ServicePhase::Queued))
            || snapshot.wait_kind == WaitKind::GrantWork
            || (snapshot.record_kind == RecordKind::ObjectRetire
                && snapshot.wait_kind == WaitKind::ObjectReclaim)
            || (snapshot.record_kind == RecordKind::VSpaceWork
                && snapshot.wait_kind == WaitKind::VSpaceWork));
    if (service_unlinked) {
        return StallClass::LostServiceKick;
    }
    if (snapshot.driver.kind == NodeRef::Kind::None
        && snapshot.expectation == Expectation::InternalFinite) {
        return StallClass::OrphanObligation;
    }
    if (is_transport_wait(snapshot.wait_kind)
        || snapshot.record_kind == RecordKind::RemoteDelivery
        || snapshot.record_kind == RecordKind::Shootdown) {
        return StallClass::TransportStall;
    }
    if (snapshot.record_kind == RecordKind::ServiceWork
        || snapshot.wait_kind == WaitKind::GrantWork) {
        return StallClass::LostServiceKick;
    }
    if (is_drain_wait(snapshot.wait_kind)
        || snapshot.record_kind == RecordKind::GrantRevoke
        || snapshot.record_kind == RecordKind::ResourceClose
        || snapshot.record_kind == RecordKind::ObjectRetire
        || snapshot.record_kind == RecordKind::VSpaceWork) {
        return StallClass::DrainStall;
    }
    if (snapshot.record_kind == RecordKind::ExecutionActor) {
        if (snapshot.phase == execution_ready_phase) {
            return StallClass::RunnableStarvation;
        }
        if (snapshot.phase == execution_throttled_phase) {
            return StallClass::TimerStall;
        }
    }
    if (snapshot.wait_kind == WaitKind::SchedulerReady
        || snapshot.wait_kind == WaitKind::SchedulerWake
        || snapshot.wait_kind == WaitKind::SchedulerActivation) {
        return StallClass::RunnableStarvation;
    }
    if (snapshot.wait_kind == WaitKind::SchedulerRefill) {
        return StallClass::TimerStall;
    }
    if (snapshot.driver.kind == NodeRef::Kind::Cpu
        || snapshot.wait_kind == WaitKind::SpinLock) {
        return StallClass::OpenOwnerStall;
    }
    return StallClass::UnclassifiedCpuStall;
}

} // namespace
#endif

auto analyze(
    NodeRef root,
    WaitGraphScratch& scratch) noexcept -> bool {
#if MYOS_CONCURRENCY_DIAG >= 1
    scratch.classification = StallClass::None;
    scratch.evidence = EvidenceGrade::None;
    scratch.count = 0;
    scratch.truncated = false;
    scratch.pending_total = 0;
    scratch.pending_shown = 0;
    scratch.pending_omitted = 0;
    for (usize index = 0; index < graph_capacity; ++index) {
        scratch.set_node(index, {});
        scratch.path_meta[index] = 0;
        scratch.fingerprints[index] = 0;
        scratch.parents[index] = 0xffU;
        scratch.edges[index] = EdgeKind::None;
    }
    if (!root) {
        scratch.classification = StallClass::Inconclusive;
        scratch.evidence = EvidenceGrade::Inconclusive;
        return false;
    }

    const auto registry = [&]() noexcept -> CpuRegistry* {
        void* const owner = arch::current_cpu_owner();
        if (owner == nullptr) {
            return nullptr;
        }
        auto& local = *static_cast<CpuLocal*>(owner);
        return local.runtime_ == nullptr
            ? nullptr : local.runtime_->owner_registry;
    }();
    const auto read_cpu = [&](NodeRef node,
                              CpuLive::Snapshot& result,
                              bool& degraded) noexcept -> bool {
        if (registry == nullptr) {
            return false;
        }
        const CpuDescriptor* const descriptor = registry->descriptor(
            CpuId{static_cast<usize>(node.identity)});
        CpuRuntime* const runtime = registry->runtime(
            CpuId{static_cast<usize>(node.identity)});
        if (descriptor == nullptr || runtime == nullptr
            || !descriptor->runtime_alive()
            || runtime->diagnostics == nullptr) {
            return false;
        }
        const auto mode = node.generation == NodeRef::cpu_generation
            ? CpuLive::SnapshotMode::Relation
            : CpuLive::SnapshotMode::Strict;
        if (!runtime->diagnostics->concurrency->live.snapshot(result, mode)) {
            return false;
        }
        degraded = result.degraded != 0;
        return true;
    };

    const auto cpu_hash = [&](const CpuLive::Snapshot& cpu,
                              bool strict_live) noexcept -> u64 {
        u64 hash = u64{0x517cc1b727220a95};
        hash = mix_hash(hash, cpu.dispatch_epoch);
        hash = mix_hash(hash, cpu.trap_entered_at);
        hash = mix_hash(hash, cpu.irq_disabled_since);
        hash = mix_hash(hash, cpu.current_actor);
        hash = mix_hash(hash, cpu.progress_epoch);
        hash = mix_hash(hash, cpu.semantic_stamp);
        hash = mix_hash(hash, cpu.context);
        hash = mix_hash(hash, cpu.trap_depth);
        hash = mix_hash(hash, cpu.irq_depth);
        hash = mix_hash(hash, cpu.degraded);
        hash = mix_hash(hash, cpu.interrupts_disabled);
        if (strict_live) {
            hash = mix_hash(hash, cpu.timer_epoch);
            hash = mix_hash(hash, cpu.activity_epoch);
            hash = mix_hash(hash, cpu.last_event_at);
        }
        hash = mix_hash(hash, cpu.has_wait);
        if (cpu.has_wait) {
            hash = mix_hash(hash, cpu.wait.wait);
            hash = mix_hash(hash, cpu.wait.subject.identity);
            hash = mix_hash(hash, static_cast<u64>(cpu.wait.subject.kind));
            hash = mix_hash(hash, cpu.wait.subject.generation);
            hash = mix_hash(hash, cpu.wait.driver.identity);
            hash = mix_hash(hash, static_cast<u64>(cpu.wait.driver.kind));
            hash = mix_hash(hash, cpu.wait.driver.generation);
            hash = mix_hash(hash, cpu.wait.obligation);
            hash = mix_hash(hash, cpu.wait.since);
            hash = mix_hash(hash, static_cast<u64>(cpu.wait.kind));
        }
        return hash;
    };

    const auto read_node = [&](NodeRef node,
                               ObservationSnapshot& observation,
                               CpuLive::Snapshot& cpu,
                               u64& fingerprint,
                               bool& degraded) noexcept -> bool {
        degraded = false;
        if (node.kind == NodeRef::Kind::Observation) {
            ObservationLease lease = ObservationLease::borrow(
                ObservationKey{node.identity});
            if (!lease || !lease.snapshot(observation)
                || observation.generation != node.generation) {
                return false;
            }
            fingerprint = snapshot_hash(observation);
            degraded = observation.evidence == EvidenceGrade::Degraded;
            return true;
        }
        if (node.kind == NodeRef::Kind::Cpu) {
            if (!read_cpu(node, cpu, degraded)) {
                return false;
            }
            if (node.generation != NodeRef::cpu_generation
                && !(
                    (cpu.irq_disabled_since != 0
                        && node.generation == cpu_stall_generation(
                            cpu.irq_disabled_since, 1))
                    || (cpu.trap_entered_at != 0
                        && node.generation == cpu_stall_generation(
                            cpu.trap_entered_at, 2)))) {
                return false;
            }
            fingerprint = cpu_hash(
                cpu, node.generation != NodeRef::cpu_generation);
            return true;
        }
        if (node.kind == NodeRef::Kind::CpuSet) {
            ObservationLease lease = ObservationLease::borrow(
                ObservationKey{node.identity});
            if (!lease || !lease.snapshot(observation)
                || observation.generation != node.generation) {
                return false;
            }
            fingerprint = snapshot_hash(observation);
            degraded = observation.evidence == EvidenceGrade::Degraded;
            return true;
        }
        fingerprint = mix_hash(
            mix_hash(static_cast<u64>(node.kind), node.identity),
            node.generation);
        return true;
    };

    const auto append = [&](NodeRef node,
                            u8 parent,
                            EdgeKind edge) noexcept -> bool {
        if (!node) {
            return true;
        }
        for (usize index = 0; index < scratch.count; ++index) {
            if (scratch.node(index) == node) {
                for (u8 ancestor = parent;
                     ancestor != 0xffU;
                     ancestor = scratch.parents[ancestor]) {
                    if (ancestor == index) {
                        scratch.classification =
                            StallClass::DeadlockCycle;
                        break;
                    }
                }
                return true;
            }
        }
        if (scratch.count >= graph_capacity) {
            scratch.truncated = true;
            return true;
        }
        scratch.set_node(scratch.count, node);
        scratch.parents[scratch.count] = parent;
        scratch.edges[scratch.count] = edge;
        ++scratch.count;
        return true;
    };

    if (!append(root, 0xffU, EdgeKind::None)) {
        scratch.evidence = EvidenceGrade::Inconclusive;
        return false;
    }
    ObservationSnapshot root_observation{};
    bool ready_publication{};
    bool degraded_graph{};
    for (usize cursor = 0; cursor < scratch.count; ++cursor) {
        const NodeRef current = scratch.node(cursor);
        ObservationSnapshot snapshot{};
        CpuLive::Snapshot cpu{};
        u64 fingerprint{};
        bool degraded{};
        if (!read_node(
                current, snapshot, cpu, fingerprint, degraded)) {
            scratch.classification = StallClass::Inconclusive;
            scratch.evidence = EvidenceGrade::Inconclusive;
            return false;
        }
        degraded_graph = degraded_graph || degraded;
        scratch.fingerprints[cursor] = fingerprint;
        const auto append_relation = [&](NodeRef node,
                                         EdgeKind edge) noexcept {
            return append(node, static_cast<u8>(cursor), edge);
        };
        if (current.kind == NodeRef::Kind::Observation) {
            if (cursor == 0) {
                root_observation = snapshot;
                ready_publication =
                    snapshot.record_kind == RecordKind::Operation
                    && snapshot.phase == static_cast<u32>(
                        OperationPhase::ReadyPublished);
                const bool scheduler_root =
                    snapshot.record_kind == RecordKind::ExecutionActor
                    && (snapshot.phase == execution_ready_phase
                        || snapshot.phase == execution_throttled_phase);
                if (scheduler_root
                    || snapshot.expectation
                        == Expectation::DeadlineBound) {
                    // Classify the root obligation before following its
                    // concrete owner.  The owner node grades the evidence;
                    // it must not replace Ready/refill/deadline semantics
                    // with the generic open-CPU endpoint class.
                    scratch.classification =
                        terminal_class(snapshot, false);
                }
            }
            // A coherent observation may publish all three relations. Keep
            // each edge explicit even when the operation-specific classifier
            // below decides which relation is the immediate cause.
            if (snapshot.wait_target
                && !append_relation(
                    NodeRef::observation(snapshot.wait_target),
                    EdgeKind::Wait)) {
                scratch.evidence = EvidenceGrade::Inconclusive;
                return false;
            }
            if (snapshot.driver
                && !append_relation(snapshot.driver, EdgeKind::Driver)) {
                scratch.evidence = EvidenceGrade::Inconclusive;
                return false;
            }
            if (snapshot.blocker
                && !append_relation(snapshot.blocker, EdgeKind::Blocker)) {
                scratch.evidence = EvidenceGrade::Inconclusive;
                return false;
            }
            if (ready_publication && cursor != 0
                && snapshot.record_kind == RecordKind::ExecutionActor
                && snapshot.phase == execution_blocked_phase
                && snapshot.wait_target
                    == ObservationKey{root.identity}) {
                const bool queued = (snapshot.detail[0] & 1U) != 0;
                const bool wake_credit = (snapshot.detail[0] & 4U) != 0;
                const bool accepted =
                    snapshot.detail[2] == root.identity
                    && (root_observation.blocker.kind
                            != NodeRef::Kind::Observation
                        || snapshot.detail[3]
                            == root_observation.blocker.identity);
                bool delivery_pending{};
                if (root_observation.blocker.kind
                    == NodeRef::Kind::Observation) {
                    ObservationSnapshot delivery{};
                    auto lease = ObservationLease::borrow(
                        ObservationKey{
                            root_observation.blocker.identity});
                    delivery_pending = lease
                        && lease.snapshot(delivery)
                        && delivery.record_kind
                            == RecordKind::RemoteDelivery
                        && delivery.generation
                            == root_observation.blocker.generation
                        && delivery.detail[0] == root.identity;
                }
                if (delivery_pending) {
                    scratch.classification =
                        StallClass::TransportStall;
                } else if (queued || wake_credit || accepted) {
                    scratch.classification =
                        StallClass::RunnableStarvation;
                } else {
                    scratch.classification = StallClass::LostWake;
                }
            } else if (scratch.classification == StallClass::None
                       && ((!snapshot.wait_target && !snapshot.driver
                            && !snapshot.blocker)
                           || snapshot.driver.kind == NodeRef::Kind::External)) {
                scratch.classification = terminal_class(snapshot, false);
            }
        } else if (current.kind == NodeRef::Kind::Cpu) {
            if (cpu.has_wait) {
                if (cpu.wait.wait
                    && !append_relation(
                        NodeRef::observation(ObservationKey{cpu.wait.wait}),
                        EdgeKind::Wait)) {
                    scratch.evidence = EvidenceGrade::Inconclusive;
                    return false;
                }
                if (cpu.wait.driver
                    && !append_relation(cpu.wait.driver, EdgeKind::Driver)) {
                    scratch.evidence = EvidenceGrade::Inconclusive;
                    return false;
                }
            } else if (scratch.classification == StallClass::None) {
                scratch.classification = StallClass::OpenOwnerStall;
            }
        } else if (current.kind == NodeRef::Kind::CpuSet) {
            bool any{};
            for (usize word = 0; word < 4; ++word) {
                const u64 pending = snapshot.detail[word];
                for (usize bit_index = 0; bit_index < 64; ++bit_index) {
                    if ((pending & (u64{1} << bit_index)) == 0) {
                        continue;
                    }
                    any = true;
                    ++scratch.pending_total;
                    const CpuId member{word * 64 + bit_index};
                    const CpuDescriptor* descriptor = registry == nullptr
                        ? nullptr : registry->descriptor(member);
                    const CpuRuntime* runtime = registry == nullptr
                        ? nullptr : registry->runtime(member);
                    const bool available = descriptor != nullptr
                        && runtime != nullptr
                        && descriptor->state() == CpuState::Online
                        && descriptor->runtime_alive()
                        && runtime->diagnostics != nullptr;
                    if (!available || scratch.count >= graph_capacity) {
                        ++scratch.pending_omitted;
                        scratch.truncated = true;
                        continue;
                    }
                    const u8 before = scratch.count;
                    if (!append_relation(NodeRef::cpu(member), EdgeKind::Member)) {
                        scratch.evidence = EvidenceGrade::Inconclusive;
                        return false;
                    }
                    if (scratch.count != before) {
                        ++scratch.pending_shown;
                    }
                }
            }
            if (any && scratch.classification == StallClass::None) {
                scratch.classification = terminal_class(snapshot, false);
            }
            if (!any && scratch.classification == StallClass::None) {
                scratch.classification = terminal_class(snapshot, false);
            }
        } else if (current.kind == NodeRef::Kind::External) {
            if (scratch.classification == StallClass::None) {
                scratch.classification = StallClass::ExternalWait;
            }
        }
    }

    // Re-read every node after the path is built.  A graph is evidence only
    // when all nodes survived one coherent diagnostic era.
    for (usize index = 0; index < scratch.count; ++index) {
        ObservationSnapshot observation{};
        CpuLive::Snapshot cpu{};
        u64 fingerprint{};
        bool degraded{};
        const bool read = read_node(
            scratch.node(index), observation, cpu, fingerprint, degraded);
        if (!read || fingerprint != scratch.fingerprints[index]) {
            scratch.classification = StallClass::Inconclusive;
            scratch.evidence = EvidenceGrade::Inconclusive;
            return false;
        }
        degraded_graph = degraded_graph || degraded;
    }
    if (scratch.classification == StallClass::None) {
        scratch.classification = StallClass::UnclassifiedCpuStall;
    }
    scratch.evidence = degraded_graph
        ? EvidenceGrade::Degraded
        : root.kind == NodeRef::Kind::External
            ? EvidenceGrade::External
            : EvidenceGrade::Confirmed;
    return scratch.evidence == EvidenceGrade::Confirmed;
#else
    static_cast<void>(root);
    static_cast<void>(scratch);
    return false;
#endif
}

auto analyze(
    ObservationKey root,
    WaitGraphScratch& scratch) noexcept -> bool {
    return analyze(NodeRef::observation(root), scratch);
}

auto stall_class_name(StallClass value) noexcept -> const char* {
    switch (value) {
    case StallClass::None:
        return "none";
    case StallClass::DeadlockCycle:
        return "deadlock-cycle";
    case StallClass::OpenOwnerStall:
        return "open-owner-stall";
    case StallClass::LostWake:
        return "lost-wake";
    case StallClass::TransportStall:
        return "transport-stall";
    case StallClass::LostServiceKick:
        return "lost-service-kick";
    case StallClass::RunnableStarvation:
        return "runnable-starvation";
    case StallClass::TimerStall:
        return "timer-stall";
    case StallClass::DeadlineDeliveryStall:
        return "deadline-delivery-stall";
    case StallClass::DrainStall:
        return "drain-stall";
    case StallClass::Livelock:
        return "livelock";
    case StallClass::OrphanObligation:
        return "orphan-obligation";
    case StallClass::ExternalWait:
        return "external-wait";
    case StallClass::UnclassifiedCpuStall:
        return "unclassified-cpu-stall";
    case StallClass::Inconclusive:
        return "inconclusive";
    }
    return "unknown";
}





CpuWaitScope::CpuWaitScope(
    WaitKind kind,
    NodeRef subject,
    NodeRef driver,
    Expectation expectation,
    SourceSite site) noexcept
    : owned_(ObservationLease::reserve(
          RecordKind::ServiceWork,
          subject.identity,
          subject.generation,
          expectation,
          site)),
      observation_(&owned_), kind_(kind), subject_(subject), driver_(driver),
      site_(site) {
    if (*observation_) {
        observation_->attempt(0, kind_, driver_);
        observation_->watch(true);
    }
    wait_token_ = set_wait(
        kind_, owned_ ? owned_.key() : ObservationKey{},
        subject_, driver_, site_);
}

CpuWaitScope::CpuWaitScope(
    ObservationLease& observation,
    WaitKind kind,
    NodeRef subject,
    NodeRef driver,
    SourceSite site) noexcept
    : observation_(&observation), kind_(kind), subject_(subject),
      driver_(driver), site_(site) {
    wait_token_ = set_wait(
        kind_, observation ? observation.key() : ObservationKey{},
        subject_, driver_, site_);
}

CpuWaitScope::~CpuWaitScope() noexcept {
    finish();
}

void CpuWaitScope::observe(u64 semantic_stamp) noexcept {
    if (observation_ == nullptr) {
        return;
    }
    if (!observed_ || semantic_stamp != last_stamp_) {
        // The borrowed observation belongs to the operation/service owner.
        // Polling this CPU-local wait must not advance that owner's semantic
        // progress or make its published phase look newer than canonical
        // state.  An owned scope is itself the observation owner.
        if (observation_ == &owned_ && owned_) {
            observation_->observe(semantic_stamp);
        } else {
            observe_wait(wait_token_, semantic_stamp);
        }
        last_stamp_ = semantic_stamp;
        observed_ = true;
    } else {
        if (observation_ == &owned_ && owned_) {
            observation_->touch(site_);
        } else {
            observe_wait(wait_token_, semantic_stamp);
        }
    }
}

void CpuWaitScope::retarget(NodeRef driver, SourceSite site) noexcept {
    if (observation_ == nullptr) {
        return;
    }
    driver_ = driver;
    site_ = site;
    if (observation_ == &owned_ && owned_) {
        observation_->attempt(0, kind_, driver_, {}, site_);
    }
    static_cast<void>(retarget_wait(wait_token_, driver_, site_));
}

void CpuWaitScope::finish() noexcept {
    if (observation_ == nullptr) {
        return;
    }
    if (wait_token_) {
        clear_wait(wait_token_);
        wait_token_ = {};
    }
    if (observation_ == &owned_) {
        owned_.finish(0);
    }
    observation_ = nullptr;
}

} // namespace kernel::diag::concurrency
