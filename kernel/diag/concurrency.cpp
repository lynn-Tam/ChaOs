#include <diag/concurrency.hpp>

#include <arch/cpu.hpp>
#include <arch/time.hpp>
#include <cpu/cpu_local.hpp>
#include <cpu/cpu_registry.hpp>
#include <cpu/cpu_runtime.hpp>
#include <diag/console.hpp>
#include <diag/panic.hpp>
#include <execution/execution.hpp>
#include <libk/manual_lifetime.hpp>
#include <libk/utility.hpp>
#include <sched/remote_queue.hpp>
#include <sync/irq_lock_guard.hpp>

#if MYOS_CONCURRENCY_PROBE == 14
#include <core/kernel_state.hpp>
#include <mm/translation.hpp>
#include <resource/pool.hpp>
#endif

#ifndef MYOS_BUILTIN_TESTS
#define MYOS_BUILTIN_TESTS 0
#endif

namespace kernel::diag::concurrency {
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

#if MYOS_BUILTIN_TESTS
// Standalone shards are used by the early built-in tests before CPU runtime
// publication. Live kernel leases always resolve through CpuRegistry; this
// bounded fallback keeps those tests on the same lease implementation.
libk::Atomic<ObservationShard*> standalone_shards[256]{};
ObservationPage standalone_page{};
#endif

#if MYOS_CONCURRENCY_PROBE
//Confirmatory experiment.
// Exit condition: remove these rendezvous variables with run_probe() when an
// external scheduler/fault harness can force the same slot interleavings.
libk::Atomic<u32> probe_joined{};
libk::Atomic<usize> probe_peer{max_cpu_count};
libk::Atomic<u32> probe_phase{};
libk::Atomic<u32> probe_errors{};
libk::Atomic<u64> probe_key{};
[[maybe_unused]] ObservationKey
    probe_fillers[ObservationShard::slot_count / 2]{};
constinit libk::ManualLifetime<sched::RemoteQueue> probe_remote_queue{};
constinit libk::ManualLifetime<sched::RemoteRequest> probe_remote_request{};
[[maybe_unused]] alignas(8) usize probe_remote_owner{};
#endif

#if MYOS_CONCURRENCY_PROBE == 14
//Confirmatory experiment.
// Exit condition: remove when an external shootdown harness can pause the
// acknowledgement callback at the same writer boundary.
libk::Atomic<u32> probe14_boot_cpu{};
libk::Atomic<u32> probe14_ack_gate{};
libk::Atomic<u32> probe14_ack_reached{};
libk::Atomic<u32> probe14_ack_written{};
libk::Atomic<u32> probe14_writer_release{};
libk::Atomic<u32> probe14_batch_done{};
libk::Atomic<u32> probe14_collision_seen{};
libk::Atomic<u64> probe14_aux_key{};
libk::Atomic<u64> probe14_resource_pool{};
libk::Atomic<u32> probe14_resource_gate{};
libk::Atomic<u32> probe14_resource_reached{};
libk::Atomic<u32> probe14_resource_release{};
libk::Atomic<u32> probe14_resource_accepted{};
libk::Atomic<u32> probe14_resource_bad{};
#endif

#if MYOS_CONCURRENCY_PROBE == 13
//Confirmatory experiment.
// Exit condition: remove when the external fault harness can deny real
// reservations at each reviewed Stage B production point.
libk::Atomic<u32> stage_b_deny_kind{
    static_cast<u32>(RecordKind::Count)};
libk::Atomic<u64> stage_b_deny_identity{};
libk::Atomic<bool> stage_b_deny_active{};

[[nodiscard]] auto denies_stage_b_reserve(
    RecordKind kind,
    u64 subject_identity) noexcept -> bool {
    return stage_b_deny_active.load<libk::MemoryOrder::Acquire>()
        && stage_b_deny_kind.load<libk::MemoryOrder::Acquire>()
            == static_cast<u32>(kind)
        && stage_b_deny_identity.load<libk::MemoryOrder::Acquire>()
            == subject_identity;
}
#endif

#if MYOS_STAGE_F_PROBE
//Confirmatory experiment.
// Exit condition: remove with confirm_dispatch() when an external recorder
// harness can correlate one real dispatcher commit with its flight identity.
libk::Atomic<u64> probe_dispatch_head[max_cpu_count]{};
libk::Atomic<u32> probe_flight_errors{};
libk::Atomic<u32> probe_flight_reported{};
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

[[nodiscard]] auto report_stall(
    CpuId watcher,
    CpuId target,
    NodeRef root,
    WatchdogCandidate::State state,
    StallClass classification,
    EvidenceGrade evidence,
    u64 age) noexcept {
    diag::console::print<
        "[concurrency] watchdog confirmed cpu={} target={} "
        "root-kind={} root={:#x} "
        "state={} class={} evidence={} age={}\n">(
        watcher.raw,
        target.raw,
        static_cast<u32>(root.kind),
        root.identity,
        static_cast<u32>(state),
        stall_class_name(classification),
        static_cast<u32>(evidence),
        age);
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

void WatchdogCandidate::publish(const StallFingerprint& value) noexcept {
    u64 odd{};
    if (!AtomicSnapshotWriter::begin(fingerprint_sequence, odd)) {
        return;
    }
    root_identity.store<libk::MemoryOrder::Relaxed>(value.root.identity);
    root_kind.store<libk::MemoryOrder::Relaxed>(
        static_cast<u32>(value.root.kind));
    root_generation.store<libk::MemoryOrder::Relaxed>(
        value.root.generation);
    fingerprint_phase.store<libk::MemoryOrder::Relaxed>(value.phase);
    fingerprint_progress.store<libk::MemoryOrder::Relaxed>(
        value.progress_epoch);
    fingerprint_activity.store<libk::MemoryOrder::Relaxed>(
        value.activity_epoch);
    relation_hash.store<libk::MemoryOrder::Relaxed>(value.relation_hash);
    driver_identity.store<libk::MemoryOrder::Relaxed>(value.driver.identity);
    driver_kind.store<libk::MemoryOrder::Relaxed>(
        static_cast<u32>(value.driver.kind));
    driver_generation.store<libk::MemoryOrder::Relaxed>(
        value.driver.generation);
    blocker_identity.store<libk::MemoryOrder::Relaxed>(value.blocker.identity);
    blocker_kind.store<libk::MemoryOrder::Relaxed>(
        static_cast<u32>(value.blocker.kind));
    blocker_generation.store<libk::MemoryOrder::Relaxed>(
        value.blocker.generation);
    AtomicSnapshotWriter::end(fingerprint_sequence, odd);
}

auto WatchdogCandidate::read(StallFingerprint& value) const noexcept -> bool {
    for (usize attempt = 0; attempt < 3; ++attempt) {
        const u64 first = AtomicSnapshotReader::begin(fingerprint_sequence);
        if ((first & 1U) != 0) {
            continue;
        }
        StallFingerprint candidate{};
        candidate.root = NodeRef{
            static_cast<NodeRef::Kind>(
                root_kind.load<libk::MemoryOrder::Relaxed>()),
            root_identity.load<libk::MemoryOrder::Relaxed>(),
            root_generation.load<libk::MemoryOrder::Relaxed>()};
        candidate.phase = fingerprint_phase.load<libk::MemoryOrder::Relaxed>();
        candidate.progress_epoch = fingerprint_progress.load<
            libk::MemoryOrder::Relaxed>();
        candidate.activity_epoch = fingerprint_activity.load<
            libk::MemoryOrder::Relaxed>();
        candidate.relation_hash = relation_hash.load<
            libk::MemoryOrder::Relaxed>();
        candidate.driver = NodeRef{
            static_cast<NodeRef::Kind>(driver_kind.load<
                libk::MemoryOrder::Relaxed>()),
            driver_identity.load<libk::MemoryOrder::Relaxed>(),
            driver_generation.load<libk::MemoryOrder::Relaxed>()};
        candidate.blocker = NodeRef{
            static_cast<NodeRef::Kind>(blocker_kind.load<
                libk::MemoryOrder::Relaxed>()),
            blocker_identity.load<libk::MemoryOrder::Relaxed>(),
            blocker_generation.load<libk::MemoryOrder::Relaxed>()};
        if (AtomicSnapshotReader::valid(fingerprint_sequence, first)) {
            value = candidate;
            return true;
        }
    }
    return false;
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
#if MYOS_BUILTIN_TESTS
    if (page_count_ == 0) {
        clear_page(standalone_page);
        storage_[0] = &standalone_page;
        page_count_ = 1;
    }
#endif
#if MYOS_BUILTIN_TESTS
    if (id_.raw < 256) {
        standalone_shards[id_.raw].store<libk::MemoryOrder::Release>(this);
    }
#endif
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
#if MYOS_BUILTIN_TESTS
    if (id_.raw < 256) {
        ObservationShard* expected = this;
        static_cast<void>(standalone_shards[id_.raw].compare_exchange_strong<
            libk::MemoryOrder::AcqRel,
            libk::MemoryOrder::Acquire>(expected, nullptr));
    }
#endif
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
#if MYOS_CONCURRENCY_PROBE == 13
    if (denies_stage_b_reserve(kind, subject_identity)) {
        return {};
    }
#endif
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
#if MYOS_BUILTIN_TESTS
    const auto resolve_standalone = [&]() noexcept -> bool {
        if (key_.shard().raw >= 256) {
            return false;
        }
        ObservationShard* const standalone = standalone_shards[
            key_.shard().raw].load<libk::MemoryOrder::Acquire>();
        if (standalone == nullptr) {
            return false;
        }
        shard = standalone;
        record = shard->valid(key_);
        return record != nullptr;
    };
#endif
    void* const owner = arch::current_cpu_owner();
    if (owner == nullptr) {
#if MYOS_BUILTIN_TESTS
        return resolve_standalone();
#else
        return false;
#endif
    }
    auto& cpu = *static_cast<CpuLocal*>(owner);
    if (cpu.runtime_ == nullptr || cpu.runtime_->owner_registry == nullptr) {
#if MYOS_BUILTIN_TESTS
        return resolve_standalone();
#else
        return false;
#endif
    }
    CpuRegistry* const registry = cpu.runtime_->owner_registry;
    shard = registry->observations(key_.shard());
    if (shard == nullptr) {
#if MYOS_BUILTIN_TESTS
        return resolve_standalone();
#else
        return false;
#endif
    }
    record = shard->valid(key_);
    if (record != nullptr) {
        return true;
    }
#if MYOS_BUILTIN_TESTS
    return resolve_standalone();
#else
    return false;
#endif
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

#if MYOS_CONCURRENCY_DIAG >= 2
void FlightRecorder::initialize(
    CpuId id,
    FlightPage* const (&storage)[page_count]) noexcept {
    id_ = id;
    head_.store<libk::MemoryOrder::Relaxed>(0);
    degraded_.store<libk::MemoryOrder::Relaxed>(0);
    wrapped_.store<libk::MemoryOrder::Relaxed>(0);
    for (usize page = 0; page < page_count; ++page) {
        pages_[page] = storage[page];
        for (auto& record : pages_[page]->records) {
            record.sequence.store<libk::MemoryOrder::Relaxed>(0);
            record.absolute_id.store<libk::MemoryOrder::Relaxed>(
                libk::numeric_limits<u64>::max());
        }
    }
}

void FlightRecorder::push(
    u64 tick,
    FlightDomain domain,
    FlightEvent event,
    u64 actor,
    u64 subject,
    u64 arg0,
    u64 arg1,
    u64 arg2,
    SourceSite site) noexcept {
    const u64 limit = libk::numeric_limits<u64>::max();
    u64 head = head_.load<libk::MemoryOrder::Relaxed>();
    for (;;) {
        if (head == limit) {
            degraded_.store<libk::MemoryOrder::Release>(1);
            return;
        }
        if (head_.compare_exchange_weak<
                libk::MemoryOrder::AcqRel,
                libk::MemoryOrder::Relaxed>(head, head + 1)) {
            break;
        }
    }
    if (head >= capacity) {
        wrapped_.store<libk::MemoryOrder::Release>(1);
    }
    const usize slot = static_cast<usize>(head % capacity);
    FlightRecord& record =
        pages_[slot / FlightPage::capacity]
            ->records[slot % FlightPage::capacity];
    u64 odd{};
    if (!AtomicSnapshotWriter::begin(record.sequence, odd)) {
        degraded_.store<libk::MemoryOrder::Release>(1);
        return;
    }
    record.absolute_id.store<libk::MemoryOrder::Relaxed>(head);
    record.tick.store<libk::MemoryOrder::Relaxed>(tick);
    record.domain.store<libk::MemoryOrder::Relaxed>(static_cast<u32>(domain));
    record.event.store<libk::MemoryOrder::Relaxed>(static_cast<u32>(event));
    record.actor.store<libk::MemoryOrder::Relaxed>(actor);
    record.subject.store<libk::MemoryOrder::Relaxed>(subject);
    record.arg0.store<libk::MemoryOrder::Relaxed>(arg0);
    record.arg1.store<libk::MemoryOrder::Relaxed>(arg1);
    record.arg2.store<libk::MemoryOrder::Relaxed>(arg2);
    record.site_file.store<libk::MemoryOrder::Relaxed>(
        reinterpret_cast<usize>(site.file));
    record.site_function.store<libk::MemoryOrder::Relaxed>(
        reinterpret_cast<usize>(site.function));
    record.site_line.store<libk::MemoryOrder::Relaxed>(site.line);
    AtomicSnapshotWriter::end(record.sequence, odd);
}

auto FlightRecorder::read(
    usize logical_index,
    FlightRecordValue& result) const noexcept -> bool {
    const u64 head = head_.load<libk::MemoryOrder::Acquire>();
    const usize count = head < capacity ? static_cast<usize>(head) : capacity;
    if (logical_index >= count) {
        return false;
    }
    const u64 absolute = head - count + logical_index;
    const usize slot = static_cast<usize>(absolute % capacity);
    const FlightRecord& record =
        pages_[slot / FlightPage::capacity]
            ->records[slot % FlightPage::capacity];
    for (usize attempt = 0; attempt < 3; ++attempt) {
        const u64 first = AtomicSnapshotReader::begin(record.sequence);
        if ((first & 1U) != 0) {
            continue;
        }
        FlightRecordValue value{};
        value.sequence = first;
        value.absolute_id = record.absolute_id.load<
            libk::MemoryOrder::Relaxed>();
        value.tick = record.tick.load<libk::MemoryOrder::Relaxed>();
        value.domain = static_cast<FlightDomain>(
            record.domain.load<libk::MemoryOrder::Relaxed>());
        value.event = static_cast<FlightEvent>(
            record.event.load<libk::MemoryOrder::Relaxed>());
        value.actor = record.actor.load<libk::MemoryOrder::Relaxed>();
        value.subject = record.subject.load<libk::MemoryOrder::Relaxed>();
        value.arg0 = record.arg0.load<libk::MemoryOrder::Relaxed>();
        value.arg1 = record.arg1.load<libk::MemoryOrder::Relaxed>();
        value.arg2 = record.arg2.load<libk::MemoryOrder::Relaxed>();
        value.site.file = reinterpret_cast<const char*>(
            record.site_file.load<libk::MemoryOrder::Relaxed>());
        value.site.function = reinterpret_cast<const char*>(
            record.site_function.load<libk::MemoryOrder::Relaxed>());
        value.site.line = record.site_line.load<libk::MemoryOrder::Relaxed>();
        if (value.absolute_id == absolute
            && AtomicSnapshotReader::valid(record.sequence, first)) {
            result = value;
            return true;
        }
    }
    return false;
}
#endif

auto current_core() noexcept -> CpuDiagnosticsCore* {
#if MYOS_CONCURRENCY_DIAG >= 1
    void* const owner = arch::current_cpu_owner();
    if (owner == nullptr) {
        return nullptr;
    }
    auto& cpu = *static_cast<CpuLocal*>(owner);
    if (cpu.runtime_ == nullptr || cpu.runtime_->diagnostics == nullptr) {
        return nullptr;
    }
    return &cpu.runtime_->diagnostics->concurrency;
#else
    return nullptr;
#endif
}

auto current_shard() noexcept -> ObservationShard* {
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
    ObservationShard* const shard = registry->observations(cpu);
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

#if MYOS_CONCURRENCY_PROBE == 13
void deny_reserves(
    RecordKind kind,
    u64 subject_identity) noexcept {
    //Confirmatory experiment.
    // Exit condition: remove with the external Stage B fault harness once it
    // can deny the same reviewed reservation scope without this hook.
    stage_b_deny_identity.store<libk::MemoryOrder::Relaxed>(
        subject_identity);
    stage_b_deny_kind.store<libk::MemoryOrder::Release>(
        static_cast<u32>(kind));
    stage_b_deny_active.store<libk::MemoryOrder::Release>(true);
}

void clear_reserve_denial() noexcept {
    //Confirmatory experiment.
    // Exit condition: remove with the external Stage B fault harness once it
    // can deny the same reviewed reservation scope without this hook.
    stage_b_deny_active.store<libk::MemoryOrder::Release>(false);
}
#endif

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
#if MYOS_STAGE_F_PROBE
        if (cpu.raw < max_cpu_count && core->flight != nullptr) {
            probe_dispatch_head[cpu.raw].store<libk::MemoryOrder::Release>(
                core->flight->head());
        }
#endif
        core->live.current_actor.store<libk::MemoryOrder::Release>(actor);
        static_cast<void>(core->live.dispatch_epoch.fetch_add<libk::MemoryOrder::Release>(1));
        core->live.last_event_at.store<libk::MemoryOrder::Release>(tick);
    }
#endif
    static_cast<void>(cpu);
    static_cast<void>(context);
}

#if MYOS_STAGE_F_PROBE
void confirm_dispatch(
    CpuId cpu,
    FlightEvent event,
    u64 outgoing,
    u64 incoming,
    u64 context,
    u64 charge,
    u64 deadline) noexcept {
    //Confirmatory experiment.
    // Exit condition: remove when an external recorder harness can correlate
    // one real dispatcher commit with its absolute flight identity.
    CpuDiagnosticsCore* const core = current_core();
    if (core == nullptr || core->flight == nullptr
        || cpu.raw >= max_cpu_count) {
        return;
    }
    FlightRecorder& flight = *core->flight;
    const u64 head = flight.head();
    const u64 before =
        probe_dispatch_head[cpu.raw].load<libk::MemoryOrder::Acquire>();
    const usize count = head < FlightRecorder::capacity
        ? static_cast<usize>(head) : FlightRecorder::capacity;
    FlightRecordValue record{};
    const bool valid = head == before + 1
        && count != 0
        && flight.read(count - 1, record)
        && record.absolute_id == head - 1
        && record.domain == FlightDomain::Scheduler
        && record.event == event
        && record.actor == outgoing
        && record.subject == incoming
        && record.arg0 == context
        && record.arg1 == charge
        && record.arg2 == deadline;
    if (!valid) {
        static_cast<void>(
            probe_flight_errors.fetch_add<libk::MemoryOrder::AcqRel>(1));
    }

    void* const owner = arch::current_cpu_owner();
    auto* const local = static_cast<CpuLocal*>(owner);
    CpuRegistry* const registry =
        local == nullptr || local->runtime_ == nullptr
        ? nullptr : local->runtime_->owner_registry;
    if (registry != nullptr && cpu == registry->boot_id()
        && head > FlightRecorder::capacity && flight.wrapped()
        && probe_flight_reported.exchange<libk::MemoryOrder::AcqRel>(1) == 0) {
        const u32 errors =
            probe_flight_errors.load<libk::MemoryOrder::Acquire>();
        diag::console::print<
            "concurrency-probe: stage-f flight-{} capacity={} head={}\n">(
                errors == 0 ? "ok" : "fail",
                FlightRecorder::capacity,
                head);
    }
}
#endif

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
#if MYOS_CONCURRENCY_PROBE == 4
    Snapshot debug_first{};
    Snapshot debug_second{};
    u32 debug_depth{};
    u32 debug_second_depth{};
#endif
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
#if MYOS_CONCURRENCY_PROBE == 4
        debug_first = first;
        debug_second = second;
        debug_depth = depth;
        debug_second_depth = second_depth;
#endif
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
#if MYOS_CONCURRENCY_PROBE == 4
    //Confirmatory experiment.
    // Exit condition: remove with the Stage B claimed-operation probe after
    // relation snapshot instability is externally observable.
    if (mode == SnapshotMode::Relation) {
        const u32 previous =
            probe_errors.fetch_or<libk::MemoryOrder::AcqRel>(1U << 29);
        if ((previous & (1U << 29)) == 0) {
            diag::console::print<
                "concurrency-probe: stage-c cpu-snapshot-rejected "
                "depth={}/{} dispatch={}/{} trap={}/{} irq={}/{} "
                "actor={:#x}/{:#x} progress={}/{} semantic={}/{} "
                "context={}/{} degraded={:#x}/{:#x}\n">(
                debug_depth,
                debug_second_depth,
                debug_first.dispatch_epoch,
                debug_second.dispatch_epoch,
                debug_first.trap_depth,
                debug_second.trap_depth,
                debug_first.irq_depth,
                debug_second.irq_depth,
                debug_first.current_actor,
                debug_second.current_actor,
                debug_first.progress_epoch,
                debug_second.progress_epoch,
                debug_first.semantic_stamp,
                debug_second.semantic_stamp,
                debug_first.context,
                debug_second.context,
                debug_first.degraded,
                debug_second.degraded);
        }
    }
#endif
    return false;
}

void watchdog_tick(CpuId cpu, u64 tick) noexcept {
#if MYOS_CONCURRENCY_DIAG >= 3
    CpuDiagnosticsCore* const watcher = current_core();
    if (watcher == nullptr) {
        return;
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
                    && runtime->diagnostics->concurrency.observations != nullptr) {
                    target_core = &runtime->diagnostics->concurrency;
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
                             WaitKind wait_kind) noexcept -> bool {
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
#if MYOS_CONCURRENCY_PROBE == 4
            //Confirmatory experiment.
            // Exit condition: remove with the Stage B claimed-operation
            // fault probe once an external runner captures analyzer evidence.
            const u32 previous =
                probe_errors.fetch_or<libk::MemoryOrder::AcqRel>(1U << 31);
            if ((previous & (1U << 31)) == 0) {
                diag::console::print<
                    "concurrency-probe: stage-c analyzer-rejected "
                    "root={:#x} evidence={} class={} count={}\n">(
                    sample.root.identity,
                    static_cast<u32>(graph.evidence),
                    static_cast<u32>(graph.classification),
                    graph.count);
            }
#endif
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
        if (action != StallAction::Record
            && report_stall(
                cpu,
                target_cpu,
                sample.root,
                confirmed,
                graph.classification,
                graph.evidence,
                age)) {
            static_cast<void>(target_core->status().flags.fetch_or<
                libk::MemoryOrder::Release>(
                DiagnosticStatus::StallReported));
        }
#if MYOS_CONCURRENCY_PROBE == 12
        //Confirmatory experiment.
        // Exit condition: remove when an external fault controller can
        // release the IRQ-off peer after the watchdog confirms it.
        if (sample.root.kind == NodeRef::Kind::Cpu && sample.phase == 1) {
            u32 expected = 1;
            static_cast<void>(probe_phase.compare_exchange_strong<
                libk::MemoryOrder::AcqRel,
                libk::MemoryOrder::Acquire>(expected, 2));
        }
#endif
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
            snapshot.wait_kind)) {
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

void mark_degraded(DiagnosticStatus::Flag flag) noexcept {
#if MYOS_CONCURRENCY_DIAG >= 1
    if (CpuDiagnosticsCore* const core = current_core(); core != nullptr) {
        static_cast<void>(core->status().flags.fetch_or<libk::MemoryOrder::Release>(flag));
        static_cast<void>(core->live.degraded.fetch_or<libk::MemoryOrder::Release>(flag));
    }
#else
    static_cast<void>(flag);
#endif
}

#if MYOS_CONCURRENCY_PROBE == 14
void probe14_shootdown_ack_before() noexcept {
    void* const owner = arch::current_cpu_owner();
    if (owner == nullptr) {
        return;
    }
    const auto& local = *static_cast<const CpuLocal*>(owner);
    if (local.descriptor == nullptr
        || local.descriptor->logical_id().raw
            == probe14_boot_cpu.load<libk::MemoryOrder::Acquire>()
        || probe14_ack_gate.load<libk::MemoryOrder::Acquire>() == 0) {
        return;
    }
    static_cast<void>(probe14_ack_reached.fetch_add<
        libk::MemoryOrder::AcqRel>(1));
    constexpr u64 spin_limit = 1U << 24;
    for (u64 spin = 0;
         probe14_ack_gate.load<libk::MemoryOrder::Acquire>() != 0;
         ++spin) {
        if (spin == spin_limit) {
            static_cast<void>(probe_errors.fetch_or<
                libk::MemoryOrder::AcqRel>(1U << 20));
            probe14_ack_gate.store<libk::MemoryOrder::Release>(0);
            break;
        }
        libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
    }
}

void probe14_shootdown_ack_after() noexcept {
    void* const owner = arch::current_cpu_owner();
    if (owner == nullptr) {
        return;
    }
    const auto& local = *static_cast<const CpuLocal*>(owner);
    if (local.descriptor == nullptr
        || local.descriptor->logical_id().raw
            == probe14_boot_cpu.load<libk::MemoryOrder::Acquire>()) {
        return;
    }
    static_cast<void>(probe14_ack_written.fetch_add<
        libk::MemoryOrder::AcqRel>(1));
    constexpr u64 spin_limit = 1U << 24;
    for (u64 spin = 0;
         probe14_writer_release.load<libk::MemoryOrder::Acquire>() == 0;
         ++spin) {
        if (spin == spin_limit) {
            static_cast<void>(probe_errors.fetch_or<
                libk::MemoryOrder::AcqRel>(1U << 21));
            probe14_writer_release.store<libk::MemoryOrder::Release>(1);
            break;
        }
        libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
    }
}

void probe14_resource_batch_after(u64 subject_identity) noexcept {
    if (subject_identity
            != probe14_resource_pool.load<libk::MemoryOrder::Acquire>()
        || probe14_resource_gate.load<libk::MemoryOrder::Acquire>() == 0) {
        return;
    }
    probe14_resource_reached.store<libk::MemoryOrder::Release>(1);
    constexpr u64 spin_limit = 1U << 24;
    for (u64 spin = 0;
         probe14_resource_release.load<libk::MemoryOrder::Acquire>() == 0;
         ++spin) {
        if (spin == spin_limit) {
            static_cast<void>(probe_errors.fetch_or<
                libk::MemoryOrder::AcqRel>(1U << 26));
            probe14_resource_release.store<libk::MemoryOrder::Release>(1);
            break;
        }
        libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
    }
}
#endif

#if MYOS_CONCURRENCY_DIAG >= 2
void dump_flight(CpuId id, const FlightRecorder& flight) noexcept {
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
            "  {} tick={} domain={} event={} actor={:#x} subject={:#x} "
            "a0={:#x} a1={:#x} a2={:#x} site={}:{}\n">(
            index,
            record_value.tick,
            static_cast<u32>(record_value.domain),
            static_cast<u32>(record_value.event),
            record_value.actor,
            record_value.subject,
            record_value.arg0,
            record_value.arg1,
            record_value.arg2,
            record_value.site.file,
            record_value.site.line);
    }
}
#endif

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
    for (usize index = 0; index < graph_capacity; ++index) {
        scratch.set_node(index, {});
        scratch.path_meta[index] = 0;
        scratch.fingerprints[index] = 0;
        scratch.parents[index] = 0xffU;
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
        if (!runtime->diagnostics->concurrency.live.snapshot(result, mode)) {
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

    const auto append = [&](NodeRef node, u8 parent) noexcept -> bool {
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
            scratch.classification = StallClass::Truncated;
            return false;
        }
        scratch.set_node(scratch.count, node);
        scratch.parents[scratch.count] = parent;
        ++scratch.count;
        return true;
    };

    if (!append(root, 0xffU)) {
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
#if MYOS_CONCURRENCY_PROBE == 4
            //Confirmatory experiment.
            // Exit condition: remove with the Stage B claimed-operation
            // fault probe after first-pass node rejection is externally
            // observable.
            const u32 previous =
                probe_errors.fetch_or<libk::MemoryOrder::AcqRel>(1U << 28);
            if ((previous & (1U << 28)) == 0) {
                diag::console::print<
                    "concurrency-probe: stage-c node-rejected "
                    "index={} kind={} identity={:#x} generation={}\n">(
                    cursor,
                    static_cast<u32>(current.kind),
                    current.identity,
                    current.generation);
            }
#endif
            scratch.classification = StallClass::Inconclusive;
            scratch.evidence = EvidenceGrade::Inconclusive;
            return false;
        }
        degraded_graph = degraded_graph || degraded;
        scratch.fingerprints[cursor] = fingerprint;
        NodeRef next{};
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
                    next = root_observation.blocker;
                } else if (queued || wake_credit || accepted) {
                    scratch.classification =
                        StallClass::RunnableStarvation;
                } else {
                    scratch.classification = StallClass::LostWake;
                }
                if (!delivery_pending) {
                    next = {};
                }
            } else {
                next = snapshot.wait_target
                    ? NodeRef::observation(snapshot.wait_target)
                    : snapshot.driver;
                if (!next
                    && snapshot.blocker.kind != NodeRef::Kind::None) {
                    next = snapshot.blocker;
                }
                if (!next
                    && scratch.classification == StallClass::None) {
                    scratch.classification =
                        terminal_class(snapshot, false);
                } else if (
                    next.kind == NodeRef::Kind::External
                    && scratch.classification == StallClass::None) {
                    scratch.classification =
                        terminal_class(snapshot, false);
                }
            }
        } else if (current.kind == NodeRef::Kind::Cpu) {
            if (cpu.has_wait) {
                next = cpu.wait.wait
                    ? NodeRef::observation(
                          ObservationKey{cpu.wait.wait})
                    : cpu.wait.driver;
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
                    if (!append(
                            NodeRef::cpu(
                                CpuId{word * 64 + bit_index}),
                            static_cast<u8>(cursor))) {
                        scratch.evidence = EvidenceGrade::Inconclusive;
                        return false;
                    }
                }
            }
            if (!any && scratch.classification == StallClass::None) {
                scratch.classification = terminal_class(snapshot, false);
            }
        } else if (current.kind == NodeRef::Kind::External) {
            if (scratch.classification == StallClass::None) {
                scratch.classification = StallClass::ExternalWait;
            }
        }
        if (next && !append(next, static_cast<u8>(cursor))) {
            scratch.evidence = EvidenceGrade::Inconclusive;
            return false;
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
#if MYOS_CONCURRENCY_PROBE == 4
            //Confirmatory experiment.
            // Exit condition: remove with the Stage B claimed-operation
            // fault probe after its graph-reread boundary is externally
            // observable.
            const u32 previous =
                probe_errors.fetch_or<libk::MemoryOrder::AcqRel>(1U << 30);
            if ((previous & (1U << 30)) == 0) {
                diag::console::print<
                    "concurrency-probe: stage-c reread-rejected "
                    "index={} kind={} read={} expected={:#x} actual={:#x}\n">(
                    index,
                    static_cast<u32>(scratch.node(index).kind),
                    read,
                    scratch.fingerprints[index],
                    fingerprint);
            }
#endif
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
    case StallClass::Truncated:
        return "truncated";
    }
    return "unknown";
}

#if MYOS_CONCURRENCY_PROBE == 14
[[nodiscard]] auto probe14_progress_negative(
    CpuId boot,
    CpuDiagnosticsCore& watcher,
    ObservationShard& target_shard) noexcept -> bool {
    //Confirmatory experiment.
    // Exit condition: remove when the direct progress witness has a
    // production harness that can prove candidate reset under concurrent
    // writers.  advance() is intentionally progress-only, so this negative
    // has no cross-field pause: every progress delta must reset the candidate
    // before activity-only classification can run.
    ObservationLease progress = target_shard.reserve(
        RecordKind::ServiceWork,
        0x14f,
        1,
        Expectation::InternalFinite,
        SourceSite::current());
    if (!progress) {
        return false;
    }
    progress.set_policy(OperationPolicy{
        .kind = WaitKind::GrantWork,
        .expectation = Expectation::InternalFinite,
        .driver = NodeRef::external(0x14f0, 1),
        .action = StallAction::Record});
    progress.transition(
        1,
        1,
        WaitKind::GrantWork,
        NodeRef::external(0x14f0, 1));
    progress.watch(true);

    ObservationSnapshot initial{};
    bool valid = progress.snapshot(initial);
    if (valid) {
        watcher.scan_cursor.store<libk::MemoryOrder::Release>(
            progress.key().slot());
        watchdog_tick(boot, initial.last_progress_at + 2);
        auto peer = ObservationLease::borrow(progress.key());
        bool seen{};
        bool livelock{};
        for (u32 iteration = 0; iteration < 64; ++iteration) {
            ObservationSnapshot before{};
            ObservationSnapshot after{};
            const bool before_ok = progress.snapshot(before);
            const u64 expected_activity = before.activity_epoch;
            const u64 expected_progress = before.progress_epoch;
            progress.advance();
            if (peer) {
                peer.advance();
            }
            const bool after_ok = progress.snapshot(after);
            valid = valid && before_ok && after_ok
                && after.activity_epoch == expected_activity
                && after.last_activity_at == before.last_activity_at
                && after.progress_epoch >= expected_progress + 2
                && after.last_progress_at >= before.last_progress_at;
            watcher.scan_cursor.store<libk::MemoryOrder::Release>(
                progress.key().slot());
            watchdog_tick(boot, initial.last_progress_at + 8 + iteration);
            for (WatchdogCandidate& candidate : watcher.candidates) {
                StallFingerprint fingerprint{};
                if (!candidate.read(fingerprint)
                    || fingerprint.root != NodeRef::observation(progress.key())) {
                    continue;
                }
                seen = true;
                livelock = livelock
                    || static_cast<WatchdogCandidate::State>(
                        candidate.state.load<libk::MemoryOrder::Acquire>())
                        == WatchdogCandidate::State::ConfirmedLivelock;
            }
        }
        valid = valid && seen && !livelock;
    }
    progress.finish(3);
    return valid;
}

#endif

#if MYOS_CONCURRENCY_PROBE
#if MYOS_CONCURRENCY_PROBE == 13
void run_probe(u32) noexcept {}
#else
void run_probe(u32 probe) noexcept {
    //Confirmatory experiment.
    // Exit condition: remove after an external kernel scenario runner can
    // deterministically pause a borrower at pin and writer boundaries.
    if (probe != 1 && probe != 5 && probe != 6 && probe != 7
        && probe != 8 && probe != 14 && (probe < 9 || probe > 12)) {
        return;
    }

    void* const owner = arch::current_cpu_owner();
    if (owner == nullptr) {
        return;
    }
    auto& local = *static_cast<CpuLocal*>(owner);
    CpuRuntime* const runtime = local.runtime_;
    CpuRegistry* const registry =
        runtime == nullptr ? nullptr : runtime->owner_registry;
    if (registry == nullptr || local.descriptor == nullptr) {
        return;
    }

    const CpuId cpu = local.descriptor->logical_id();
    const CpuId boot = registry->boot_id();
#if MYOS_CONCURRENCY_PROBE == 14
    if (probe == 14) {
        //Confirmatory experiment.
        // Exit condition: remove when an external four-hart scenario runner
        // can pause the real ShootdownTicket acknowledgement and the
        // production Observation/Watchdog boundaries independently.
        constexpr u32 done_phase = 99;
        constexpr u32 batch_phase = 10;
        constexpr u32 production_phase = 15;
        constexpr u32 collision_phase = 20;
        constexpr u32 resume_phase = 21;
        constexpr u32 spin_limit = 1U << 24;
        const auto wait_phase = [&](u32 wanted) noexcept -> bool {
            for (u32 spin = 0; spin < spin_limit; ++spin) {
                if (probe_phase.load<libk::MemoryOrder::Acquire>() == wanted) {
                    return true;
                }
                libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
            }
            static_cast<void>(probe_errors.fetch_or<
                libk::MemoryOrder::AcqRel>(1U << 22));
            return false;
        };

        static_cast<void>(probe_joined.fetch_add<libk::MemoryOrder::AcqRel>(1));
        bool joined = false;
        for (u32 spin = 0; spin < spin_limit; ++spin) {
            if (probe_joined.load<libk::MemoryOrder::Acquire>()
                == registry->count()) {
                joined = true;
                break;
            }
            libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
        }
        if (!joined) {
            static_cast<void>(probe_errors.fetch_or<
                libk::MemoryOrder::AcqRel>(1U << 23));
            if (cpu == boot) {
                probe_phase.store<libk::MemoryOrder::Release>(done_phase);
            }
            return;
        }

        CpuId target{(boot.raw + 1) % registry->count()};
        for (usize attempt = 0; attempt < registry->count(); ++attempt) {
            const CpuDescriptor* const descriptor =
                registry->descriptor(target);
            const CpuRuntime* const target_runtime = registry->runtime(target);
            if (descriptor != nullptr && target_runtime != nullptr
                && descriptor->state() == CpuState::Online
                && target_runtime->diagnostics != nullptr
                && target_runtime->diagnostics->concurrency.observations
                    != nullptr) {
                break;
            }
            target = CpuId{(target.raw + 1) % registry->count()};
        }

        if (cpu != boot) {
            if (cpu != target) {
                static_cast<void>(wait_phase(done_phase));
                return;
            }
            // The target CPU is the concurrent production publisher.  It
            // remains in this bounded loop so the boot CPU can snapshot its
            // shard and force a writer collision without synthetic storage.
            for (u32 spin = 0; spin < spin_limit * 8U; ++spin) {
                const u32 phase = probe_phase.load<libk::MemoryOrder::Acquire>();
                if (phase == batch_phase) {
                    const ObservationKey key{
                        probe_key.load<libk::MemoryOrder::Acquire>()};
                    auto observation = ObservationLease::borrow(key);
                    if (!observation) {
                        static_cast<void>(probe_errors.fetch_or<
                            libk::MemoryOrder::AcqRel>(1U << 24));
                        probe14_batch_done.store<libk::MemoryOrder::Release>(1);
                        continue;
                    }
                    for (u64 epoch = 1; epoch <= 128; ++epoch) {
                        ObservationBatch update{
                            .phase = static_cast<u32>(epoch),
                            .semantic_stamp = epoch,
                            .wait = WaitKind::GrantWork,
                            .driver = NodeRef::external(0x14b0, 1),
                            .blocker = NodeRef::external(0x14b1, 1),
                            .site = SourceSite::current(),
                            .detail_mask = 0xfU,
                            .update_progress = true,
                            .update_watched = true,
                            .watched = true};
                        update.detail[0] = epoch;
                        update.detail[1] = epoch ^ 0xa5a5U;
                        update.detail[2] = epoch + 17;
                        update.detail[3] = update.detail[0]
                            ^ update.detail[1] ^ update.detail[2];
                        observation.publish(update);
                        for (u32 pause = 0; pause < 32; ++pause) {
                            libk::atomic_signal_fence<
                                libk::MemoryOrder::SeqCst>();
                        }
                    }
                    probe14_batch_done.store<libk::MemoryOrder::Release>(1);
                    while (probe_phase.load<libk::MemoryOrder::Acquire>()
                        == batch_phase) {
                        libk::atomic_signal_fence<
                            libk::MemoryOrder::SeqCst>();
                    }
                    continue;
                }
                if (phase == production_phase) {
                    const u64 pool_raw = probe14_resource_pool.load<
                        libk::MemoryOrder::Acquire>();
                    const auto* pool = reinterpret_cast<
                        const kernel::resource::ResourcePool*>(pool_raw);
                    if (pool != nullptr) {
                        const ObservationKey key =
                            pool->close_observation_key_for_probe();
                        auto observation = ObservationLease::borrow(key);
                        ObservationSnapshot snapshot{};
                        if (observation && observation.snapshot(snapshot)) {
                            u64 stamp = snapshot.phase;
                            stamp = stamp * 131 + snapshot.detail[0];
                            stamp = stamp * 131 + snapshot.detail[1];
                            stamp = stamp * 131 + snapshot.detail[2];
                            stamp = stamp * 131 + snapshot.detail[3];
                            const bool coherent =
                                snapshot.record_kind
                                    == RecordKind::ResourceClose
                                && snapshot.subject_identity == pool_raw
                                && snapshot.generation != 0
                                && snapshot.phase
                                    <= static_cast<u32>(
                                        kernel::resource::PoolState::Closed)
                                && snapshot.semantic_stamp == stamp
                                && snapshot.evidence
                                    != EvidenceGrade::Degraded;
                            if (!coherent) {
                                static_cast<void>(probe14_resource_bad.fetch_add<
                                    libk::MemoryOrder::AcqRel>(1));
                            } else {
                                const u32 accepted =
                                    probe14_resource_accepted.fetch_add<
                                        libk::MemoryOrder::AcqRel>(1) + 1;
                                if (accepted >= 16) {
                                    probe14_resource_release.store<
                                        libk::MemoryOrder::Release>(1);
                                }
                            }
                        }
                    }
                    continue;
                }
                if (phase == collision_phase) {
                    const ObservationKey key{
                        probe14_aux_key.load<libk::MemoryOrder::Acquire>()};
                    auto observation = ObservationLease::borrow(key);
                    if (observation) {
                        observation.transition(
                            2,
                            2,
                            WaitKind::GrantWork,
                            NodeRef::external(0x14c0, 1));
                    }
                    probe14_collision_seen.store<libk::MemoryOrder::Release>(1);
                    while (probe_phase.load<libk::MemoryOrder::Acquire>()
                        == collision_phase) {
                        libk::atomic_signal_fence<
                            libk::MemoryOrder::SeqCst>();
                    }
                    continue;
                }
                if (phase == done_phase) {
                    return;
                }
                libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
            }
            static_cast<void>(probe_errors.fetch_or<
                libk::MemoryOrder::AcqRel>(1U << 25));
            return;
        }

        probe14_boot_cpu.store<libk::MemoryOrder::Release>(boot.raw);
        probe_joined.store<libk::MemoryOrder::Release>(registry->count());
        probe_phase.store<libk::MemoryOrder::Release>(0);
        probe_errors.store<libk::MemoryOrder::Release>(0);
        probe14_ack_gate.store<libk::MemoryOrder::Release>(1);
        probe14_ack_reached.store<libk::MemoryOrder::Release>(0);
        probe14_ack_written.store<libk::MemoryOrder::Release>(0);
        probe14_writer_release.store<libk::MemoryOrder::Release>(0);

        CpuDiagnosticsCore* const watcher = current_core();
        CpuRuntime* const target_runtime = registry->runtime(target);
        CpuDiagnosticsCore* const target_core =
            target_runtime == nullptr || target_runtime->diagnostics == nullptr
            ? nullptr : &target_runtime->diagnostics->concurrency;
        ObservationShard* const boot_shard = registry->observations(boot);
        ObservationShard* const target_shard = registry->observations(target);
        bool ok = registry->count() >= 4 && watcher != nullptr
            && watcher->flight != nullptr && target_core != nullptr
            && boot_shard != nullptr && target_shard != nullptr;
        u32 errors = ok ? 0 : 1U;
        u32 resource_final_state{};

        // Exercise the actual TranslationState -> ShootdownPlan ->
        // ShootdownTicket path.  Every online CPU enters the temporary state,
        // so the published target set is the live four-hart set rather than a
        // synthetic observation key.
        mm::TranslationState translation{};
        CpuSet shootdown_targets{};
        if (ok) {
            for (usize index = 0; index < registry->count(); ++index) {
                const CpuId id{index};
                static_cast<void>(shootdown_targets.insert(id));
                static_cast<void>(translation.enter(id));
            }
            auto mutation = translation.begin();
            auto plan = mm::ShootdownPlan::prepare(
                *registry, boot, shootdown_targets);
            mm::ShootdownTicket ticket{};
            if (!mutation || !plan) {
                ok = false;
                errors |= 1U << 1;
                probe14_ack_gate.store<libk::MemoryOrder::Release>(0);
                probe14_writer_release.store<libk::MemoryOrder::Release>(1);
            } else {
                static_cast<void>(mutation.value().commit(
                    libk::move(plan).value(), ticket));
                const u32 remote_count =
                    static_cast<u32>(registry->count() - 1);
                bool reached = false;
                for (u32 spin = 0; spin < spin_limit; ++spin) {
                    if (probe14_ack_reached.load<
                            libk::MemoryOrder::Acquire>() >= remote_count) {
                        reached = true;
                        break;
                    }
                    libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
                }
                if (!reached) {
                    ok = false;
                    errors |= 1U << 2;
                }
                const ObservationKey key{ticket.observation_key()};
                ObservationRecord* pinned{};
                u64 odd{};
                const bool held = reached && boot_shard->pin(key, pinned)
                    && AtomicSnapshotWriter::begin(pinned->sequence, odd);
                if (!held) {
                    ok = false;
                    errors |= 1U << 3;
                }
                probe14_ack_gate.store<libk::MemoryOrder::Release>(0);
                bool written = false;
                for (u32 spin = 0; spin < spin_limit; ++spin) {
                    if (probe14_ack_written.load<
                            libk::MemoryOrder::Acquire>() >= remote_count) {
                        written = true;
                        break;
                    }
                    libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
                }
                if (!written) {
                    ok = false;
                    errors |= 1U << 4;
                }
                if (held) {
                    bool equal = true;
                    for (usize word = 0; word < CpuSet::word_count; ++word) {
                        u64 expected{};
                        for (usize index = 0; index < registry->count(); ++index) {
                            const CpuId id{index};
                            if (shootdown_targets.contains(id)
                                && !ticket.acknowledged(id)) {
                                expected |= u64{1} << (id.raw % CpuSet::word_bits);
                            }
                        }
                        equal = equal
                            && pinned->detail[word].load<
                                libk::MemoryOrder::Acquire>() == expected;
                    }
                    if (!equal) {
                        ok = false;
                        errors |= 1U << 5;
                    }
                    AtomicSnapshotWriter::end(pinned->sequence, odd);
                    boot_shard->unpin(key, *pinned);
                }
                probe14_writer_release.store<libk::MemoryOrder::Release>(1);
                bool complete = false;
                for (u32 spin = 0; spin < spin_limit; ++spin) {
                    if (ticket.complete()) {
                        complete = true;
                        break;
                    }
                    static_cast<void>(mm::retry_shootdowns(*registry, ticket));
                    libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
                }
                if (!complete) {
                    ok = false;
                    errors |= 1U << 6;
                }
            }
            for (usize index = 0; index < registry->count(); ++index) {
                translation.leave(CpuId{index});
            }
        }

        // Batch coherence is driven by the same diagnostics batch contract
        // used by ResourcePool/VSpace writers.  The target hart publishes a
        // complete epoch while this hart accepts snapshots; any mixed tuple
        // is a failed observation even if the sample itself is rejected.
        ObservationLease batch{};
        ObservationKey batch_key{};
        if (ok) {
            batch = target_shard->reserve(
                RecordKind::ResourceClose,
                0x14b,
                1,
                Expectation::InternalFinite,
                SourceSite::current());
            batch_key = batch.detach_key();
            probe_key.store<libk::MemoryOrder::Release>(batch_key.raw);
            probe14_batch_done.store<libk::MemoryOrder::Release>(0);
            probe_phase.store<libk::MemoryOrder::Release>(batch_phase);
            usize accepted{};
            usize rejected{};
            for (u32 spin = 0; spin < spin_limit; ++spin) {
                ObservationSnapshot snapshot{};
                if (target_shard->snapshot(batch_key, snapshot)) {
                    const u64 epoch = snapshot.detail[0];
                    // A newly reserved record has an all-zero reset state.
                    // It is not one of the worker's published epochs and
                    // must not be classified as a mixed batch.
                    if (epoch == 0) {
                        libk::atomic_signal_fence<
                            libk::MemoryOrder::SeqCst>();
                        continue;
                    }
                    ++accepted;
                    const bool coherent = snapshot.phase == epoch
                        && snapshot.semantic_stamp == epoch
                        && snapshot.detail[1] == (epoch ^ 0xa5a5U)
                        && snapshot.detail[2] == epoch + 17
                        && snapshot.detail[3] == (snapshot.detail[0]
                            ^ snapshot.detail[1] ^ snapshot.detail[2]);
                    if (!coherent) {
                        ok = false;
                        errors |= 1U << 7;
                    }
                } else {
                    ++rejected;
                }
                if (probe14_batch_done.load<libk::MemoryOrder::Acquire>() != 0
                    && accepted >= 16) {
                    break;
                }
                libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
            }
            if (probe14_batch_done.load<libk::MemoryOrder::Acquire>() == 0
                || accepted < 16 || rejected > spin_limit) {
                ok = false;
                errors |= 1U << 8;
            }
            probe_phase.store<libk::MemoryOrder::Release>(resume_phase);
            ObservationLease::borrow(batch_key).finish(1);
        }

        // Exercise the real ResourcePool close publisher.  The production
        // method captures state and all four counters under its lifecycle
        // lock, then emits one ObservationBatch.  Its probe-only hook pauses
        // immediately after that publication so the target hart can read the
        // canonical epoch, rather than publishing a second synthetic record.
        if (ok) {
            bool production_ok = false;
            if (runtime->kernel != nullptr) {
                auto made = runtime->kernel->objects().create_resource(
                    kernel::resource::Budget{
                        .memory = kernel::mm::page_size,
                        .caps = 1});
                if (made) {
                    auto production_hold =
                        libk::move(made).value().publish();
                    auto& production_pool = production_hold.get();
                    probe14_resource_pool.store<libk::MemoryOrder::Release>(
                        reinterpret_cast<u64>(&production_pool));
                    probe14_resource_gate.store<libk::MemoryOrder::Release>(
                        1);
                    probe14_resource_reached.store<
                        libk::MemoryOrder::Release>(0);
                    probe14_resource_release.store<
                        libk::MemoryOrder::Release>(0);
                    probe14_resource_accepted.store<
                        libk::MemoryOrder::Release>(0);
                    probe14_resource_bad.store<libk::MemoryOrder::Release>(0);
                    probe_phase.store<libk::MemoryOrder::Release>(
                        production_phase);
                    const auto final_state = production_pool.close();
                    resource_final_state = static_cast<u32>(final_state);
                    probe14_resource_gate.store<libk::MemoryOrder::Release>(
                        0);
                    const u32 accepted =
                        probe14_resource_accepted.load<
                            libk::MemoryOrder::Acquire>();
                    production_ok =
                        probe14_resource_reached.load<
                            libk::MemoryOrder::Acquire>() != 0
                        && accepted >= 16
                        && probe14_resource_bad.load<
                            libk::MemoryOrder::Acquire>() == 0
                        && final_state == kernel::resource::PoolState::Closed;
                    if (!production_ok) {
                        errors |= 1U << 15;
                    }
                    probe_phase.store<libk::MemoryOrder::Release>(
                        resume_phase);
                    probe14_resource_pool.store<
                        libk::MemoryOrder::Release>(0);
                } else {
                    errors |= 1U << 15;
                }
            } else {
                errors |= 1U << 15;
            }
            if (!production_ok) {
                ok = false;
            }
        }

        // Force one real sequence collision on the target shard, retire that
        // generation, then prove a later generation is graded from its own
        // evidence rather than the shard's sticky health bit.
        ObservationLease collision{};
        ObservationKey collision_key{};
        if (ok) {
            collision = target_shard->reserve(
                RecordKind::ServiceWork,
                0x14c,
                1,
                Expectation::InternalFinite,
                SourceSite::current());
            collision_key = collision.detach_key();
            probe14_aux_key.store<libk::MemoryOrder::Release>(collision_key.raw);
            ObservationRecord* pinned{};
            u64 odd{};
            const bool held = target_shard->pin(collision_key, pinned)
                && AtomicSnapshotWriter::begin(pinned->sequence, odd);
            if (!held) {
                ok = false;
                errors |= 1U << 9;
            } else {
                probe14_collision_seen.store<libk::MemoryOrder::Release>(0);
                probe_phase.store<libk::MemoryOrder::Release>(collision_phase);
                bool collided = false;
                for (u32 spin = 0; spin < spin_limit; ++spin) {
                    if (probe14_collision_seen.load<
                            libk::MemoryOrder::Acquire>() != 0) {
                        collided = true;
                        break;
                    }
                    libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
                }
                if (!collided) {
                    ok = false;
                    errors |= 1U << 10;
                }
                AtomicSnapshotWriter::end(pinned->sequence, odd);
                target_shard->unpin(collision_key, *pinned);
            }
            probe_phase.store<libk::MemoryOrder::Release>(resume_phase);
            ObservationSnapshot degraded_snapshot{};
            const bool degraded_snapshot_ok =
                target_shard->snapshot(collision_key, degraded_snapshot)
                && degraded_snapshot.evidence == EvidenceGrade::Degraded
                && target_shard->degraded();
            if (!degraded_snapshot_ok) {
                ok = false;
                errors |= 1U << 11;
            }
            ObservationLease::borrow(collision_key).finish(2);
        }

        ObservationLease recovery{};
        ObservationLease positive{};
        WatchdogPolicy saved_policy{};
        bool recovery_confirmed{};
        bool recovery_livelock{};
        bool positive_livelock{};
        bool progress_only_ok{};
        if (ok) {
            saved_policy = target_core->policy;
            target_core->policy.critical_soft = 2;
            target_core->policy.critical_hard = 4;
            target_core->policy.transport_soft = 2;
            target_core->policy.transport_hard = 4;
            target_core->policy.service_soft = 2;
            target_core->policy.service_hard = 4;
            target_core->policy.scheduler_soft = 2;
            target_core->policy.scheduler_hard = 4;
            watcher->scan_cursor.store<libk::MemoryOrder::Release>(0);
            watcher->candidate_cursor.store<libk::MemoryOrder::Release>(0);
            for (WatchdogCandidate& candidate : watcher->candidates) {
                candidate.state.store<libk::MemoryOrder::Release>(
                    static_cast<u32>(WatchdogCandidate::State::Clear));
                candidate.active_intervals.store<
                    libk::MemoryOrder::Release>(0);
            }
            recovery = target_shard->reserve(
                RecordKind::ServiceWork,
                0x14d,
                1,
                Expectation::InternalFinite,
                SourceSite::current());
            positive = target_shard->reserve(
                RecordKind::ServiceWork,
                0x14e,
                1,
                Expectation::InternalFinite,
                SourceSite::current());
            const auto prepare = [](ObservationLease& observation,
                                    u64 identity) noexcept {
                observation.set_policy(OperationPolicy{
                    .kind = WaitKind::GrantWork,
                    .expectation = Expectation::InternalFinite,
                    .driver = NodeRef::external(identity, 1),
                    .action = StallAction::Record});
                observation.transition(
                    1,
                    1,
                    WaitKind::GrantWork,
                    NodeRef::external(identity, 1));
                observation.watch(true);
            };
            progress_only_ok = probe14_progress_negative(
                boot, *watcher, *target_shard);
            if (!progress_only_ok) {
                errors |= 1U << 16;
            }
            if (!recovery || !positive) {
                ok = false;
                errors |= 1U << 12;
            } else {
                prepare(recovery, 0x14d0);
                prepare(positive, 0x14e0);
                ObservationSnapshot recovery_before{};
                const bool clean = recovery.snapshot(recovery_before)
                    && recovery_before.evidence != EvidenceGrade::Degraded
                    && target_shard->degraded();
                if (!clean) {
                    ok = false;
                    errors |= 1U << 13;
                }
                const u64 recovery_anchor = recovery_before.last_progress_at;
                watchdog_tick(boot, recovery_anchor + 2);
                recovery.touch(); // exactly one activity blip
                for (u32 tick = 0; tick < 96; ++tick) {
                    watchdog_tick(boot, recovery_anchor + 8 + tick);
                }
                const u64 positive_anchor = [&]() noexcept -> u64 {
                    ObservationSnapshot snapshot{};
                    return positive.snapshot(snapshot)
                        ? snapshot.last_progress_at : 0;
                }();
                for (u32 tick = 0; tick < 96; ++tick) {
                    positive.touch();
                    watchdog_tick(boot, positive_anchor + 2 + tick);
                }
                const u64 head = watcher->flight->head();
                const usize count = head < FlightRecorder::capacity
                    ? static_cast<usize>(head) : FlightRecorder::capacity;
                for (usize index = 0; index < count; ++index) {
                    FlightRecordValue event{};
                    if (!watcher->flight->read(index, event)) {
                        continue;
                    }
                    if (event.subject == recovery.key().raw) {
                        recovery_confirmed = recovery_confirmed
                            || event.event == FlightEvent::WatchdogConfirmed;
                        recovery_livelock = recovery_livelock
                            || event.event == FlightEvent::WatchdogLivelock;
                    }
                    if (event.subject == positive.key().raw) {
                        positive_livelock = positive_livelock
                            || event.event == FlightEvent::WatchdogLivelock;
                    }
                }
                WaitGraphScratch graph{};
                const bool analyzed = analyze(recovery.key(), graph)
                    && graph.evidence == EvidenceGrade::Confirmed;
                if (!recovery_confirmed || recovery_livelock
                    || !positive_livelock || !analyzed) {
                    ok = false;
                    errors |= 1U << 14;
                }
            }
            target_core->policy = saved_policy;
        }
        if (recovery) {
            recovery.finish(3);
        }
        if (positive) {
            positive.finish(3);
        }
        ok = ok && progress_only_ok;
        probe_phase.store<libk::MemoryOrder::Release>(done_phase);
        const u32 global_errors = probe_errors.load<libk::MemoryOrder::Acquire>();
        errors |= global_errors;
        const u32 resource_reached =
            probe14_resource_reached.load<libk::MemoryOrder::Acquire>();
        const u32 resource_accepted =
            probe14_resource_accepted.load<libk::MemoryOrder::Acquire>();
        const u32 resource_bad =
            probe14_resource_bad.load<libk::MemoryOrder::Acquire>();
        diag::console::print<
            "concurrency-probe: stage-c evidence-{} errors=0x{:x} "
            "batch={} resource={} reached={} accepted={} bad={} "
            "final_state={} progress-only={} degraded-recovery={} "
            "one-blip={} persistent={}\n">(
            ok ? "ok" : "fail",
            errors,
            probe14_batch_done.load<libk::MemoryOrder::Acquire>(),
            resource_accepted,
            resource_reached,
            resource_accepted,
            resource_bad,
            resource_final_state,
            progress_only_ok,
            target_shard != nullptr && target_shard->degraded(),
            recovery_confirmed && !recovery_livelock,
            positive_livelock);
        return;
    }
#endif
#if MYOS_CONCURRENCY_PROBE != 14
    if (probe >= 9 && probe <= 11) {
        //Confirmatory experiment.
        // Exit condition: remove when the external scheduler harness can
        // hold the real root Binding at Ready/Throttled/deadline boundaries.
        static_cast<void>(
            probe_joined.fetch_add<libk::MemoryOrder::AcqRel>(1));
        while (probe_joined.load<libk::MemoryOrder::Acquire>()
            != registry->count()) {
            libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
        }
        if (cpu == boot) {
            probe_phase.store<libk::MemoryOrder::Release>(1);
        } else {
            while (probe_phase.load<libk::MemoryOrder::Acquire>() != 1) {
                libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
            }
        }
        return;
    }
    if (probe == 12) {
        //Confirmatory experiment.
        // Exit condition: remove when an external fault harness can
        // sequentially hold peers at the IrqToken and trap-exit boundaries.
        static_cast<void>(
            probe_joined.fetch_add<libk::MemoryOrder::AcqRel>(1));
        while (probe_joined.load<libk::MemoryOrder::Acquire>()
            != registry->count()) {
            libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
        }
        if (registry->count() < 4) {
            if (cpu == boot) {
                diag::console::print<
                    "concurrency-probe: stage-g cpu-live-fail cpus={}\n">(
                    registry->count());
            }
            return;
        }
        const CpuId irq_target{
            (boot.raw + 1) % registry->count()};
        const CpuId trap_target{
            (boot.raw + 3) % registry->count()};
        if (cpu == boot) {
            diag::console::print<
                "concurrency-probe: stage-g cpu-live-ok irq={} trap={}\n">(
                irq_target.raw, trap_target.raw);
            probe_phase.store<libk::MemoryOrder::Release>(1);
            return;
        }
        while (probe_phase.load<libk::MemoryOrder::Acquire>() == 0) {
            libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
        }
        if (cpu == irq_target) {
            kernel::sync::IrqToken token{};
            while (probe_phase.load<libk::MemoryOrder::Acquire>() == 1) {
                libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
            }
        }
        if (cpu == trap_target) {
            while (probe_phase.load<libk::MemoryOrder::Acquire>() != 2) {
                libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
            }
        }
        return;
    }
    if (probe == 8) {
        //Confirmatory experiment.
        // Exit condition: remove when the external service-fault harness can
        // withhold the real notifier and scheduler acceptance independently.
        static_cast<void>(
            probe_joined.fetch_add<libk::MemoryOrder::AcqRel>(1));
        while (probe_joined.load<libk::MemoryOrder::Acquire>()
            != registry->count()) {
            libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
        }
        if (cpu != boot) {
            while (probe_phase.load<libk::MemoryOrder::Acquire>() != 2) {
                libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
            }
            return;
        }

        ObservationShard* const shard = registry->observations(boot);
        bool ok = shard != nullptr;
        u32 errors = ok ? 0 : 1U;
        ObservationLease service{};
        ObservationLease actor{};
        if (ok) {
            service = shard->reserve(
                RecordKind::ServiceWork,
                0xe801,
                1,
                Expectation::InternalFinite,
                SourceSite::current());
            actor = shard->reserve(
                RecordKind::ExecutionActor,
                0xe802,
                1,
                Expectation::SchedulerControlled,
                SourceSite::current());
            ok = service && actor;
            if (!ok) {
                errors |= 1U << 1;
            }
        }
        if (ok) {
            service.attempt(
                static_cast<u32>(ServicePhase::Queued),
                WaitKind::SchedulerWake,
                {});
            service.watch(true);
            WaitGraphScratch graph{};
            const bool lost = analyze(service.key(), graph)
                && graph.classification == StallClass::LostServiceKick
                && graph.count == 1;
            if (!lost) {
                errors |= 1U << 2;
            }

            actor.transition(
                execution_ready_phase,
                execution_ready_phase,
                WaitKind::SchedulerReady,
                NodeRef::external(0xe8ee, 1));
            service.attempt(
                static_cast<u32>(ServicePhase::WakeIssued),
                WaitKind::SchedulerWake,
                NodeRef::observation(actor.key()));
            graph = {};
            const bool ready = analyze(service.key(), graph)
                && graph.classification == StallClass::RunnableStarvation
                && graph.count == 3;
            if (!ready) {
                errors |= 1U << 3;
            }
            ok = lost && ready;
        }
        if (service) {
            service.finish(static_cast<u32>(ServicePhase::Completed));
        }
        if (actor) {
            actor.finish(execution_ready_phase);
        }
        diag::console::print<
            "concurrency-probe: stage-e analyzer-{} errors=0x{:x}\n">(
            ok ? "ok" : "fail", errors);
        probe_phase.store<libk::MemoryOrder::Release>(2);
        return;
    }
    if (probe == 7) {
        //Confirmatory experiment.
        // Exit condition: remove when the external kernel fault harness can
        // stop the real remote scheduler path at every transport phase and
        // inspect exact operation/request generations.
        static_cast<void>(
            probe_joined.fetch_add<libk::MemoryOrder::AcqRel>(1));
        while (probe_joined.load<libk::MemoryOrder::Acquire>()
            != registry->count()) {
            libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
        }
        if (cpu != boot) {
            while (probe_phase.load<libk::MemoryOrder::Acquire>() != 2) {
                libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
            }
            return;
        }

        ObservationShard* const shard = registry->observations(boot);
        bool ok = shard != nullptr;
        u32 errors = ok ? 0 : 1U;
        ObservationLease operation{};
        ObservationLease other{};
        ObservationLease actor{};
        if (ok) {
            operation = shard->reserve(
                RecordKind::Operation,
                0xd701,
                7,
                Expectation::InternalFinite,
                SourceSite::current());
            other = shard->reserve(
                RecordKind::Operation,
                0xd702,
                8,
                Expectation::InternalFinite,
                SourceSite::current());
            actor = shard->reserve(
                RecordKind::ExecutionActor,
                0xd703,
                1,
                Expectation::SchedulerControlled,
                SourceSite::current());
            ok = operation && other && actor;
            if (!ok) {
                errors |= 1U << 1;
            }
        }

        auto& queue = probe_remote_queue.emplace(boot);
        auto& request = probe_remote_request.emplace(
            sched::RemoteKind::Wake, &probe_remote_owner);
        sched::RemotePostResult posted{};
        kernel::IpiDelivery::Token first{};
        ObservationKey projected_cause{};
        if (ok) {
            posted = queue.post(request, operation.key());
            ObservationSnapshot delivery{};
            auto delivery_lease =
                ObservationLease::borrow(posted.delivery);
            projected_cause = request.diagnostic_cause(posted.delivery);
            const bool posted_ok =
                posted.disposition == sched::RemotePost::Inserted
                && request.pending()
                && delivery_lease.snapshot(delivery)
                && delivery.record_kind == RecordKind::RemoteDelivery
                && delivery.phase
                    == static_cast<u32>(RemotePhase::NeedsKick)
                && delivery.detail[0] == operation.key().raw
                && delivery.detail[1]
                    == reinterpret_cast<u64>(&probe_remote_owner)
                && delivery.detail[2] == boot.raw
                // The home-CPU drain path uses this same bounded projection
                // as Binding::publish_accept(); a single cause must survive
                // the delivery snapshot even though it is not canonical.
                && projected_cause == operation.key();
            if (!posted_ok) {
                errors |= 1U << 2;
            }
            ok = ok && posted_ok;

            const auto same = queue.post(request, operation.key());
            const auto other_post = queue.post(request, other.key());
            const auto coalesced_cause = request.diagnostic_cause(
                posted.delivery);
            const bool coalesce_ok =
                same.disposition == sched::RemotePost::Coalesced
                && same.delivery == posted.delivery
                && other_post.disposition == sched::RemotePost::Coalesced
                && other_post.delivery == posted.delivery
                // The bounded projection retains the first concrete cause.
                // Later causes remain flight evidence; no per-request
                // multi-cause truth feeds the analyzer yet.
                && coalesced_cause == operation.key();
            if (!coalesce_ok) {
                errors |= 1U << 3;
            }
            ok = ok && coalesce_ok;

            const auto claimed = queue.claim_transport();
            if (claimed) {
                first = *claimed;
            }
            delivery = {};
            const bool flight_ok = claimed
                && delivery_lease.snapshot(delivery)
                && delivery.phase == static_cast<u32>(
                    RemotePhase::InFlight)
                && delivery.detail[3] == claimed->generation;
            if (!flight_ok) {
                errors |= 1U << 4;
            }
            ok = ok && flight_ok;

            queue.transport_failed(first);
            delivery = {};
            const bool retry_ok = delivery_lease.snapshot(delivery)
                && delivery.phase == static_cast<u32>(
                    RemotePhase::Retry);
            const auto retry = queue.claim_transport();
            if (!retry_ok || !retry
                || retry->generation == first.generation) {
                errors |= 1U << 5;
                ok = false;
            }

            sched::RemoteRequest* const taken = queue.take();
            delivery = {};
            const bool taken_ok = taken == &request
                && delivery_lease.snapshot(delivery)
                && delivery.phase == static_cast<u32>(
                    RemotePhase::Taken);
            if (!taken_ok) {
                errors |= 1U << 6;
            }
            ok = ok && taken_ok;
            queue.accepted(request, true);
            delivery = {};
            const bool accepted_ok = delivery_lease.snapshot(delivery)
                && delivery.phase == static_cast<u32>(
                    RemotePhase::Accepted);
            if (!accepted_ok) {
                errors |= 1U << 7;
            }
            ok = ok && accepted_ok;
        }

        if (request.pending()) {
            queue.complete(request);
        }
        if (posted.delivery
            && ObservationLease::borrow(posted.delivery)) {
            errors |= 1U << 8;
            ok = false;
        }

        if (operation && actor) {
            actor.transition(
                execution_blocked_phase,
                execution_blocked_phase,
                WaitKind::OperationCompletion,
                NodeRef::observation(operation.key()));
            actor.link_wait(
                operation.key(),
                WaitKind::OperationCompletion,
                NodeRef::observation(operation.key()));
            operation.publish(
                OperationPhase::ReadyPublished,
                NodeRef::observation(actor.key()));
            WaitGraphScratch graph{};
            const bool lost = analyze(operation.key(), graph)
                && graph.evidence == EvidenceGrade::Confirmed
                && graph.classification == StallClass::LostWake;
            if (!lost) {
                errors |= 1U << 9;
            }
            ok = ok && lost;

            // Feed the projected cause into the same witness slot that
            // Binding::publish_accept() updates after a remote drain.
            actor.detail(2, projected_cause.raw);
            graph = {};
            const bool accepted = analyze(operation.key(), graph)
                && graph.classification
                    == StallClass::RunnableStarvation;
            if (!accepted) {
                errors |= 1U << 10;
            }
            ok = ok && accepted;

            actor.detail(2, 0);
            const auto transport = queue.post(
                request, operation.key());
            operation.publish(
                OperationPhase::ReadyPublished,
                NodeRef::observation(actor.key()),
                NodeRef::observation(transport.delivery));
            graph = {};
            const bool pending = analyze(operation.key(), graph)
                && graph.classification == StallClass::TransportStall;
            if (!pending) {
                errors |= 1U << 11;
            }
            ok = ok && pending;
            static_cast<void>(queue.cancel(request));
        }

        if (operation) {
            operation.finish(
                static_cast<u32>(OperationPhase::Finished));
        }
        if (other) {
            other.finish(
                static_cast<u32>(OperationPhase::Finished));
        }
        if (actor) {
            actor.finish(1);
        }
        probe_remote_request.reset();
        probe_remote_queue.reset();
        diag::console::print<
            "concurrency-probe: stage-d delivery-{} errors=0x{:x}\n">(
            ok ? "ok" : "fail", errors);
        probe_phase.store<libk::MemoryOrder::Release>(2);
        return;
    }
    if (probe == 6) {
        //Confirmatory experiment.
        // Exit condition: remove when an external scenario runner can drive
        // multiple real watchdog roots through stable, active and progressing
        // intervals while retaining the per-CPU flight evidence.
        static_cast<void>(
            probe_joined.fetch_add<libk::MemoryOrder::AcqRel>(1));
        while (probe_joined.load<libk::MemoryOrder::Acquire>()
            != registry->count()) {
            libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
        }
        if (cpu != boot) {
            while (probe_phase.load<libk::MemoryOrder::Acquire>() != 2) {
                libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
            }
            return;
        }

        CpuId target = boot;
        if (registry->count() > 1) {
            CpuId candidate{(boot.raw + 1) % registry->count()};
            for (usize attempt = 0; attempt < registry->count(); ++attempt) {
                const CpuDescriptor* const descriptor =
                    registry->descriptor(candidate);
                CpuRuntime* const candidate_runtime =
                    registry->runtime(candidate);
                if (descriptor != nullptr
                    && candidate_runtime != nullptr
                    && descriptor->state() == CpuState::Online
                    && candidate_runtime->diagnostics != nullptr
                    && candidate_runtime->diagnostics->concurrency.observations
                        != nullptr) {
                    target = candidate;
                    break;
                }
                candidate =
                    CpuId{(candidate.raw + 1) % registry->count()};
            }
        }
        CpuRuntime* const target_runtime = registry->runtime(target);
        CpuDiagnosticsCore* const watcher = current_core();
        CpuDiagnosticsCore* const target_core =
            target_runtime == nullptr || target_runtime->diagnostics == nullptr
            ? nullptr : &target_runtime->diagnostics->concurrency;
        ObservationShard* const shard = registry->observations(target);
        bool ok = watcher != nullptr && target_core != nullptr
            && shard != nullptr && watcher->flight != nullptr;
        u32 errors = ok ? 0 : 1U;
        constexpr usize roots = 6;
        ObservationLease leases[roots]{};
        ObservationKey keys[roots]{};
        u64 anchor{};
        WatchdogPolicy saved_policy{};
        if (ok) {
            saved_policy = target_core->policy;
            target_core->policy.service_soft = 2;
            target_core->policy.service_hard = 4;
            for (usize index = 0; index < roots; ++index) {
                leases[index] = shard->reserve(
                    RecordKind::ServiceWork,
                    0xc600 + index,
                    1,
                    Expectation::InternalFinite,
                    SourceSite::current());
                if (!leases[index]) {
                    ok = false;
                    errors |= 1U << 1;
                    break;
                }
                leases[index].set_policy(OperationPolicy{
                    .kind = WaitKind::GrantWork,
                    .expectation = Expectation::InternalFinite,
                    .driver = NodeRef::external(0xc6ee, 1),
                    .action = StallAction::Record,
                });
                leases[index].transition(
                    1,
                    1,
                    WaitKind::GrantWork,
                    NodeRef::external(0xc6ee, 1));
                leases[index].watch(true);
                keys[index] = leases[index].key();
                ObservationSnapshot snapshot{};
                if (!leases[index].snapshot(snapshot)) {
                    ok = false;
                    errors |= 1U << 2;
                    break;
                }
                if (snapshot.last_progress_at > anchor) {
                    anchor = snapshot.last_progress_at;
                }
            }
        }

        u32 confirmed{};
        bool livelock{};
        bool noisy_confirmed{};
        if (ok) {
            watcher->scan_cursor.store<libk::MemoryOrder::Release>(0);
            watcher->candidate_cursor.store<libk::MemoryOrder::Release>(0);
            for (WatchdogCandidate& candidate : watcher->candidates) {
                candidate.state.store<libk::MemoryOrder::Release>(
                    static_cast<u32>(WatchdogCandidate::State::Clear));
            }
            for (usize iteration = 0; iteration < 160; ++iteration) {
                leases[4].touch(SourceSite::current());
                leases[5].advance();
                watchdog_tick(boot, anchor + 10 + iteration);
                const u64 flight_head = watcher->flight->head();
                const usize flight_count =
                    flight_head < FlightRecorder::capacity
                    ? static_cast<usize>(flight_head)
                    : FlightRecorder::capacity;
                for (usize record_index = 0;
                     record_index < flight_count;
                     ++record_index) {
                    FlightRecordValue record{};
                    if (!watcher->flight->read(record_index, record)) {
                        continue;
                    }
                    for (usize root_index = 0;
                         root_index < roots;
                         ++root_index) {
                        if (record.subject != keys[root_index].raw) {
                            continue;
                        }
                        if (record.event == FlightEvent::WatchdogConfirmed) {
                            confirmed |= 1U << root_index;
                            noisy_confirmed =
                                noisy_confirmed || root_index == 5;
                        } else if (
                            record.event == FlightEvent::WatchdogLivelock
                            && root_index == 4) {
                            livelock = true;
                        }
                    }
                }
            }
            const bool stable_ok = (confirmed & 0xfU) == 0xfU;
            const bool interval_ok = livelock && !noisy_confirmed;
            if (!stable_ok) {
                errors |= 1U << 3;
            }
            if (!interval_ok) {
                errors |= 1U << 4;
            }
            ok = ok && stable_ok && interval_ok;

            WaitGraphScratch graph{};
            const bool external_ok =
                !analyze(NodeRef::external(0xc6ff, 1), graph)
                && graph.evidence == EvidenceGrade::External
                && graph.classification == StallClass::ExternalWait;
            graph = {};
            const bool stale_cpu_ok =
                !analyze(
                    NodeRef{
                        NodeRef::Kind::Cpu,
                        target.raw,
                        cpu_stall_generation(123, 1)},
                    graph)
                && graph.evidence == EvidenceGrade::Inconclusive;
            if (!external_ok) {
                errors |= 1U << 5;
            }
            if (!stale_cpu_ok) {
                errors |= 1U << 6;
            }
            ok = ok && external_ok && stale_cpu_ok;
        }
        if (target_core != nullptr) {
            target_core->policy = saved_policy;
        }
        if (!ok && watcher != nullptr) {
            StallFingerprint seen[CpuDiagnosticsCore::candidate_capacity]{};
            for (usize index = 0;
                 index < CpuDiagnosticsCore::candidate_capacity;
                 ++index) {
                static_cast<void>(
                    watcher->candidates[index].read(seen[index]));
            }
            diag::console::print<
                "concurrency-probe: stage-c watchdog-debug watched={:#x} "
                "scan={} roots={:#x},{:#x},{:#x},{:#x} "
                "states={},{},{},{}\n">(
                shard == nullptr ? 0 : shard->watched(),
                watcher->scan_cursor.load<libk::MemoryOrder::Acquire>(),
                seen[0].root.identity,
                seen[1].root.identity,
                seen[2].root.identity,
                seen[3].root.identity,
                watcher->candidates[0].state.load<
                    libk::MemoryOrder::Acquire>(),
                watcher->candidates[1].state.load<
                    libk::MemoryOrder::Acquire>(),
                watcher->candidates[2].state.load<
                    libk::MemoryOrder::Acquire>(),
                watcher->candidates[3].state.load<
                    libk::MemoryOrder::Acquire>());
        }
        for (ObservationLease& lease : leases) {
            if (lease) {
                lease.finish(1);
            }
        }
        diag::console::print<
            "concurrency-probe: stage-c watchdog-{} errors=0x{:x} "
            "confirmed=0x{:x} livelock={}\n">(
            ok ? "ok" : "fail",
            errors,
            confirmed,
            livelock);
        probe_phase.store<libk::MemoryOrder::Release>(2);
        return;
    }
    if (probe == 5) {
        //Confirmatory experiment.
        // Exit condition: remove when an external kernel scenario can hold
        // producer CPUs at open leaves and publish a multi-member CpuSet.
        static_cast<void>(
            probe_joined.fetch_add<libk::MemoryOrder::AcqRel>(1));
        while (probe_joined.load<libk::MemoryOrder::Acquire>()
            != registry->count()) {
            libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
        }
        if (cpu != boot) {
            while (probe_phase.load<libk::MemoryOrder::Acquire>() != 2) {
                libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
            }
            return;
        }

        ObservationShard* const shard = registry->observations(boot);
        bool ok = shard != nullptr;
        u32 errors = ok ? 0 : 1U;
        ObservationLease external{};
        ObservationLease set{};
        if (ok) {
            external = shard->reserve(
                RecordKind::ServiceWork,
                0xc501,
                1,
                Expectation::InternalFinite,
                SourceSite::current());
            set = shard->reserve(
                RecordKind::Shootdown,
                0xc502,
                1,
                Expectation::InternalFinite,
                SourceSite::current());
            ok = external && set;
            if (!ok) {
                errors |= 1U << 1;
            }
        }
        if (ok) {
            external.transition(
                1,
                1,
                WaitKind::GrantWork,
                NodeRef::external(0xc5ee, 1));
            u64 pending[4]{};
            for (usize index = 0; index < registry->count(); ++index) {
                pending[index / 64] |= u64{1} << (index % 64);
            }
            for (usize word = 0; word < 4; ++word) {
                set.detail(word, pending[word]);
            }

            WaitGraphScratch graph{};
            CpuId producer = registry->count() > 1
                ? CpuId{(boot.raw + 1) % registry->count()} : boot;
            const bool cpu_ok = analyze(NodeRef::cpu(producer), graph)
                && graph.evidence == EvidenceGrade::Confirmed
                && graph.classification == StallClass::OpenOwnerStall
                && graph.count == 1;
            if (!cpu_ok) {
                errors |= 1U << 2;
            }
            ok = ok && cpu_ok;
            graph = {};
            const bool external_ok = analyze(external.key(), graph)
                && graph.evidence == EvidenceGrade::Confirmed
                && graph.classification == StallClass::LostServiceKick
                && graph.count == 2;
            if (!external_ok) {
                errors |= 1U << 3;
                diag::console::print<
                    "concurrency-probe: stage-c external "
                    "class={} evidence={} count={} degraded={}\n">(
                    static_cast<u32>(graph.classification),
                    static_cast<u32>(graph.evidence),
                    graph.count,
                    shard->degraded());
            }
            ok = ok && external_ok;
            graph = {};
            const bool set_ok = analyze(
                    NodeRef::cpu_set(set.key()), graph)
                && graph.evidence == EvidenceGrade::Confirmed
                && graph.count == registry->count() + 1;
            if (!set_ok) {
                errors |= 1U << 4;
            }
            ok = ok && set_ok;

            ObservationSnapshot before{};
            ObservationSnapshot after{};
            ok = ok && external.snapshot(before);
            external.touch(SourceSite::current());
            const bool hash_ok = external.snapshot(after)
                && snapshot_hash(before, HashMode::Coherent)
                    != snapshot_hash(after, HashMode::Coherent)
                && snapshot_hash(before, HashMode::Relation)
                    == snapshot_hash(after, HashMode::Relation);
            if (!hash_ok) {
                errors |= 1U << 5;
            }
            ok = ok && hash_ok;
        }
        if (external) {
            external.finish(1);
        }
        if (set) {
            set.finish(1);
        }
        diag::console::print<
            "concurrency-probe: stage-c analyzer-{} errors=0x{:x}\n">(
            ok ? "ok" : "fail", errors);
        probe_phase.store<libk::MemoryOrder::Release>(2);
        return;
    }

    bool peer_role{};
    if (cpu != boot) {
        usize expected = max_cpu_count;
        peer_role = probe_peer.compare_exchange_strong<
            libk::MemoryOrder::AcqRel,
            libk::MemoryOrder::Acquire>(expected, cpu.raw)
            || expected == cpu.raw;
    }

    static_cast<void>(
        probe_joined.fetch_add<libk::MemoryOrder::AcqRel>(1));
    while (probe_joined.load<libk::MemoryOrder::Acquire>()
        != registry->count()) {
        libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
    }
    if (cpu != boot && !peer_role) {
        return;
    }

    ObservationShard* const shard = registry->observations(boot);
    if (shard == nullptr) {
        static_cast<void>(
            probe_errors.fetch_or<libk::MemoryOrder::Release>(1U));
        return;
    }

    if (peer_role) {
        while (probe_phase.load<libk::MemoryOrder::Acquire>() != 1) {
            libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
        }
        const ObservationKey old{
            probe_key.load<libk::MemoryOrder::Acquire>()};
        ObservationRecord* pinned{};
        if (!shard->pin(old, pinned)) {
            static_cast<void>(
                probe_errors.fetch_or<libk::MemoryOrder::Release>(1U << 1));
            return;
        }
        probe_phase.store<libk::MemoryOrder::Release>(2);
        while (probe_phase.load<libk::MemoryOrder::Acquire>() != 3) {
            libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
        }
        shard->unpin(old, *pinned);
        probe_phase.store<libk::MemoryOrder::Release>(4);

        while (probe_phase.load<libk::MemoryOrder::Acquire>() != 5) {
            libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
        }
        const ObservationKey current{
            probe_key.load<libk::MemoryOrder::Acquire>()};
        ObservationRecord* writing{};
        u64 odd{};
        if (!shard->pin(current, writing)
            || !AtomicSnapshotWriter::begin(writing->sequence, odd)
            || !shard->active(current, *writing)) {
            static_cast<void>(
                probe_errors.fetch_or<libk::MemoryOrder::Release>(1U << 2));
            if (writing != nullptr) {
                shard->unpin(current, *writing);
            }
            return;
        }
        probe_phase.store<libk::MemoryOrder::Release>(6);
        while (probe_phase.load<libk::MemoryOrder::Acquire>() != 7) {
            libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
        }
        writing->phase.store<libk::MemoryOrder::Relaxed>(0x77);
        AtomicSnapshotWriter::end(writing->sequence, odd);
        shard->unpin(current, *writing);
        probe_phase.store<libk::MemoryOrder::Release>(8);
        return;
    }

    constexpr usize ephemeral_capacity = ObservationShard::slot_count / 2;
    for (usize index = 0; index < ephemeral_capacity; ++index) {
        ObservationLease lease = shard->reserve(
            RecordKind::Operation,
            index + 1,
            1,
            Expectation::InternalFinite,
            SourceSite::current());
        if (!lease) {
            static_cast<void>(
                probe_errors.fetch_or<libk::MemoryOrder::Release>(1U << 3));
            break;
        }
        probe_fillers[index] = lease.detach_key();
    }

    ObservationLease actor = shard->reserve(
        RecordKind::ExecutionActor,
        0xace,
        1,
        Expectation::SchedulerControlled,
        SourceSite::current());
    if (!actor || actor.key().slot() >= ephemeral_capacity) {
        static_cast<void>(
            probe_errors.fetch_or<libk::MemoryOrder::Release>(1U << 4));
    }

    CpuDiagnosticsCore* const core = current_core();
    const u32 initial_depth = core == nullptr ? 0
        : core->live.wait_depth.load<libk::MemoryOrder::Acquire>();
    {
        CpuWaitScope wait{
            WaitKind::CompletionPublication,
            NodeRef::external(0xcafe),
            NodeRef::cpu(boot),
            Expectation::InternalFinite};
        CpuLive::WaitSnapshot top{};
        if (core == nullptr
            || core->live.wait_depth.load<libk::MemoryOrder::Acquire>()
                != initial_depth + 1
            || !core->live.top_wait(top)
            || top.subject != NodeRef::external(0xcafe)) {
            static_cast<void>(
                probe_errors.fetch_or<libk::MemoryOrder::Release>(1U << 5));
        }
        wait.retarget(NodeRef::external(0xbeef));
        if (core == nullptr
            || core->live.wait_depth.load<libk::MemoryOrder::Acquire>()
                != initial_depth + 1
            || !core->live.top_wait(top)
            || top.driver != NodeRef::external(0xbeef)) {
            static_cast<void>(
                probe_errors.fetch_or<libk::MemoryOrder::Release>(1U << 6));
        }
    }
    if (core == nullptr
        || core->live.wait_depth.load<libk::MemoryOrder::Acquire>()
            != initial_depth) {
        static_cast<void>(
            probe_errors.fetch_or<libk::MemoryOrder::Release>(1U << 7));
    }

    const ObservationKey old = probe_fillers[0];
    if (registry->count() == 1) {
        shard->release(old);
    } else {
        probe_key.store<libk::MemoryOrder::Release>(old.raw);
        probe_phase.store<libk::MemoryOrder::Release>(1);
        while (probe_phase.load<libk::MemoryOrder::Acquire>() != 2) {
            libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
        }
        shard->release(old);
        ObservationLease premature = shard->reserve(
            RecordKind::Operation,
            0xbad,
            1,
            Expectation::InternalFinite,
            SourceSite::current());
        if (premature) {
            static_cast<void>(
                probe_errors.fetch_or<libk::MemoryOrder::Release>(1U << 8));
            premature.finish(1);
        }
        probe_phase.store<libk::MemoryOrder::Release>(3);
        while (probe_phase.load<libk::MemoryOrder::Acquire>() != 4) {
            libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
        }
    }

    ObservationLease current = shard->reserve(
        RecordKind::Operation,
        0xf123,
        2,
        Expectation::InternalFinite,
        SourceSite::current());
    if (!current || current.key().slot() != old.slot()
        || current.key().generation() != old.generation() + 1) {
        static_cast<void>(
            probe_errors.fetch_or<libk::MemoryOrder::Release>(1U << 9));
    }
    ObservationLease stale = ObservationLease::borrow(old);
    stale.transition(
        0x66,
        0x66,
        WaitKind::Unknown,
        NodeRef::external(0x66));
    stale.finish(0x66);
    ObservationSnapshot snapshot{};
    if (!current.snapshot(snapshot)
        || snapshot.subject_identity != 0xf123
        || snapshot.phase != 0) {
        static_cast<void>(
            probe_errors.fetch_or<libk::MemoryOrder::Release>(1U << 10));
    }

    if (registry->count() == 1) {
        current.finish(1);
    } else {
        const ObservationKey current_key = current.detach_key();
        probe_key.store<libk::MemoryOrder::Release>(current_key.raw);
        probe_phase.store<libk::MemoryOrder::Release>(5);
        while (probe_phase.load<libk::MemoryOrder::Acquire>() != 6) {
            libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
        }
        ObservationLease::borrow(current_key).finish(1);
        if (shard->valid(current_key) != nullptr) {
            static_cast<void>(
                probe_errors.fetch_or<libk::MemoryOrder::Release>(1U << 11));
        }
        probe_phase.store<libk::MemoryOrder::Release>(7);
        while (probe_phase.load<libk::MemoryOrder::Acquire>() != 8) {
            libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
        }
        ObservationLease next = shard->reserve(
            RecordKind::Operation,
            0xe123,
            3,
            Expectation::InternalFinite,
            SourceSite::current());
        if (!next || next.key().slot() != current_key.slot()
            || next.key().generation() != current_key.generation() + 1) {
            static_cast<void>(
                probe_errors.fetch_or<libk::MemoryOrder::Release>(1U << 12));
        }
        next.finish(1);
    }

    for (usize index = 1; index < ephemeral_capacity; ++index) {
        shard->release(probe_fillers[index]);
    }
    actor.finish(1);

    const u32 errors = probe_errors.load<libk::MemoryOrder::Acquire>();
    if (errors == 0) {
        diag::console::print<"concurrency-probe: stage-a ok cpus={}\n">(
            registry->count());
    } else {
        diag::console::print<
            "concurrency-probe: stage-a fail errors=0x{:x} cpus={}\n">(
            errors, registry->count());
    }
#endif
}
#endif
#endif

#if MYOS_CONCURRENCY_PROBE == 12
auto trap_exit_stall_for_probe() noexcept -> bool {
    //Confirmatory experiment.
    // Exit condition: remove when an external fault harness can stop a peer
    // after trap handling while preserving its live trap-enter publication.
    if (probe_phase.load<libk::MemoryOrder::Acquire>() != 2) {
        return false;
    }
    void* const owner = arch::current_cpu_owner();
    if (owner == nullptr) {
        return false;
    }
    const auto& local = *static_cast<const CpuLocal*>(owner);
    const CpuRegistry* const registry =
        local.runtime_ == nullptr ? nullptr : local.runtime_->owner_registry;
    if (registry == nullptr || registry->count() < 4
        || local.descriptor == nullptr) {
        return false;
    }
    const CpuId trap_target{
        (registry->boot_id().raw + 3) % registry->count()};
    return local.descriptor->logical_id() == trap_target;
}
#endif

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
