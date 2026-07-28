#include <diag/concurrency.hpp>

#include <arch/cpu.hpp>
#include <arch/time.hpp>
#include <cpu/cpu_local.hpp>
#include <cpu/cpu_registry.hpp>
#include <cpu/cpu_runtime.hpp>
#include <diag/console.hpp>
#include <libk/utility.hpp>

#ifndef MYOS_BUILTIN_TESTS
#define MYOS_BUILTIN_TESTS 0
#endif

namespace kernel::diag::concurrency {
namespace {

constexpr u64 active_flag = 1;
constexpr u64 generation_mask = (u64{1} << 40) - 1;
constexpr u64 lease_generation_shift = 24;
constexpr u64 release_requested = u64{1} << 23;
constexpr u64 pin_count_mask = release_requested - 1;
constexpr u64 lease_generation_mask = generation_mask << lease_generation_shift;

[[nodiscard]] constexpr auto lease_generation(u64 generation) noexcept -> u64 {
    return generation << lease_generation_shift;
}

#if MYOS_BUILTIN_TESTS
// Standalone shards are used by the early built-in tests before CPU runtime
// publication. Live kernel leases always resolve through CpuRegistry; this
// bounded fallback keeps those tests on the same lease implementation.
libk::Atomic<ObservationShard*> standalone_shards[256]{};
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
    u64 soft{};
    u64 hard{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return soft != 0 && hard >= soft;
    }
};

[[nodiscard]] auto thresholds_for(
    const WatchdogPolicy& policy,
    const ObservationSnapshot& snapshot) noexcept -> WatchdogThresholds {
    if (snapshot.expectation == Expectation::ExternalUnbounded
        || snapshot.expectation == Expectation::Idle
        || snapshot.expectation == Expectation::ObserveOnly) {
        return {};
    }

    switch (snapshot.wait_kind) {
    case WaitKind::SpinLock:
    case WaitKind::CompletionPublication:
    case WaitKind::OperationCompletion:
        return {policy.critical_soft, policy.critical_hard};
    case WaitKind::RemoteRequest:
    case WaitKind::IpiDelivery:
    case WaitKind::ShootdownAck:
        return {policy.transport_soft, policy.transport_hard};
    case WaitKind::SchedulerReady:
    case WaitKind::SchedulerRefill:
    case WaitKind::SchedulerWake:
    case WaitKind::SchedulerActivation:
        return {policy.scheduler_soft, policy.scheduler_hard};
    default:
        break;
    }
    if (snapshot.expectation == Expectation::SchedulerControlled) {
        return {policy.scheduler_soft, policy.scheduler_hard};
    }
    return {policy.service_soft, policy.service_hard};
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
    record.flags.store<libk::MemoryOrder::Relaxed>(0);
    record.lease_state.store<libk::MemoryOrder::Relaxed>(0);
    record.activity_epoch.store<libk::MemoryOrder::Relaxed>(0);
    record.progress_epoch.store<libk::MemoryOrder::Relaxed>(0);
    record.started_at.store<libk::MemoryOrder::Relaxed>(0);
    record.last_activity_at.store<libk::MemoryOrder::Relaxed>(0);
    record.last_progress_at.store<libk::MemoryOrder::Relaxed>(0);
    record.record_kind.store<libk::MemoryOrder::Relaxed>(0);
    record.phase.store<libk::MemoryOrder::Relaxed>(0);
    record.wait_kind.store<libk::MemoryOrder::Relaxed>(0);
    record.expectation.store<libk::MemoryOrder::Relaxed>(0);
    record.subject_identity.store<libk::MemoryOrder::Relaxed>(0);
    record.subject_generation.store<libk::MemoryOrder::Relaxed>(0);
    record.parent_key.store<libk::MemoryOrder::Relaxed>(0);
    record.waiter_key.store<libk::MemoryOrder::Relaxed>(0);
    record.driver_key.store<libk::MemoryOrder::Relaxed>(0);
    record.driver_kind.store<libk::MemoryOrder::Relaxed>(0);
    record.driver_generation.store<libk::MemoryOrder::Relaxed>(0);
    record.blocker_key.store<libk::MemoryOrder::Relaxed>(0);
    record.blocker_kind.store<libk::MemoryOrder::Relaxed>(0);
    record.blocker_generation.store<libk::MemoryOrder::Relaxed>(0);
    record.semantic_stamp.store<libk::MemoryOrder::Relaxed>(0);
    record.site_file.store<libk::MemoryOrder::Relaxed>(0);
    record.site_function.store<libk::MemoryOrder::Relaxed>(0);
    record.site_line.store<libk::MemoryOrder::Relaxed>(0);
    for (auto& detail : record.detail) {
        detail.store<libk::MemoryOrder::Relaxed>(0);
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
    fingerprint_key.store<libk::MemoryOrder::Relaxed>(value.key.raw);
    fingerprint_generation.store<libk::MemoryOrder::Relaxed>(value.generation);
    fingerprint_phase.store<libk::MemoryOrder::Relaxed>(value.phase);
    fingerprint_progress.store<libk::MemoryOrder::Relaxed>(
        value.progress_epoch);
    fingerprint_activity.store<libk::MemoryOrder::Relaxed>(
        value.activity_epoch);
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
        candidate.key = ObservationKey{
            fingerprint_key.load<libk::MemoryOrder::Relaxed>()};
        candidate.generation = fingerprint_generation.load<
            libk::MemoryOrder::Relaxed>();
        candidate.phase = fingerprint_phase.load<libk::MemoryOrder::Relaxed>();
        candidate.progress_epoch = fingerprint_progress.load<
            libk::MemoryOrder::Relaxed>();
        candidate.activity_epoch = fingerprint_activity.load<
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
    LatencyProfile* profile) noexcept {
    id_ = id;
    profile_ = profile;
    if (profile_ != nullptr) {
        profile_->initialize();
    }
#if MYOS_BUILTIN_TESTS
    if (id_.raw < 256) {
        standalone_shards[id_.raw].store<libk::MemoryOrder::Release>(this);
    }
#endif
    allocated_.store<libk::MemoryOrder::Relaxed>(0);
    watched_.store<libk::MemoryOrder::Relaxed>(0);
    degraded_.store<libk::MemoryOrder::Relaxed>(0);
    for (usize index = 0; index < slot_count; ++index) {
        generations[index].store<libk::MemoryOrder::Relaxed>(0);
        clear_record(records[index]);
    }
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
        ObservationSnapshot snapshot{};
        const u64 generation = records[index].generation.load<
            libk::MemoryOrder::Acquire>();
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

auto ObservationShard::valid(ObservationKey key) const noexcept
    -> ObservationRecord* {
    const usize index = key.slot();
    if (!key || key.shard() != id_ || index >= slot_count) {
        return nullptr;
    }
    const u64 allocated = allocated_.load<libk::MemoryOrder::Acquire>();
    if ((allocated & bit(index)) == 0) {
        return nullptr;
    }
    ObservationRecord& record = const_cast<ObservationRecord&>(records[index]);
    if (record.generation.load<libk::MemoryOrder::Acquire>()
            != key.generation()
        || (record.flags.load<libk::MemoryOrder::Acquire>() & active_flag)
            == 0) {
        return nullptr;
    }
    return &record;
}

auto ObservationShard::pin(
    ObservationKey key,
    ObservationRecord*& result) const noexcept -> bool {
    result = nullptr;
    ObservationRecord* const record = valid(key);
    if (record == nullptr) {
        return false;
    }
    const u64 tag = lease_generation(key.generation());
    u64 state = record->lease_state.load<libk::MemoryOrder::Acquire>();
    for (;;) {
        if ((state & lease_generation_mask) != tag
            || (state & release_requested) != 0
            || (state & pin_count_mask) == pin_count_mask) {
            return false;
        }
        if (record->lease_state.compare_exchange_weak<
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
    const u64 state = record.lease_state.fetch_sub<
        libk::MemoryOrder::AcqRel>(1);
    if ((state & pin_count_mask) == 0
        || (state & lease_generation_mask) != lease_generation(
               key.generation())) {
        const_cast<ObservationShard*>(this)->mark_degraded();
        return;
    }
    if ((state & pin_count_mask) != 1
        || (state & release_requested) == 0) {
        return;
    }

    // The last pin completes a release requested by the owner.  No reserve
    // can observe the slot free until both the active flag and bitmap clear.
    if (record.generation.load<libk::MemoryOrder::Acquire>()
            != key.generation()
        || (record.flags.load<libk::MemoryOrder::Acquire>() & active_flag)
            == 0) {
        return;
    }
    const usize index = key.slot();
    if (index >= slot_count) {
        return;
    }
    static_cast<void>(const_cast<ObservationShard*>(this)->watched_.fetch_and<
        libk::MemoryOrder::Release>(~bit(index)));
    record.flags.store<libk::MemoryOrder::Release>(0);
    static_cast<void>(const_cast<ObservationShard*>(this)->allocated_.fetch_and<
        libk::MemoryOrder::Release>(~bit(index)));
}

auto ObservationShard::reserve(
    RecordKind kind,
    u64 subject_identity,
    u64 subject_generation,
    Expectation expectation,
    SourceSite site) noexcept -> ObservationLease {
    for (usize index = 0; index < slot_count; ++index) {
        const u64 mask = bit(index);
        u64 observed = allocated_.load<libk::MemoryOrder::Relaxed>();
        if ((observed & mask) != 0) {
            continue;
        }
        if (!allocated_.compare_exchange_weak<
                libk::MemoryOrder::AcqRel,
                libk::MemoryOrder::Acquire>(observed, observed | mask)) {
            if (index != 0) {
                --index;
            }
            continue;
        }

        u64 generation = generations[index].load<libk::MemoryOrder::Relaxed>();
        if (generation >= generation_mask) {
            static_cast<void>(allocated_.fetch_and<libk::MemoryOrder::Release>(~mask));
            degraded_.store<libk::MemoryOrder::Release>(1);
            continue;
        }
        ++generation;
        generations[index].store<libk::MemoryOrder::Release>(generation);

        ObservationRecord& record = records[index];
        clear_record(record);
        record.generation.store<libk::MemoryOrder::Relaxed>(generation);
        record.lease_state.store<libk::MemoryOrder::Release>(
            lease_generation(generation));
        record.flags.store<libk::MemoryOrder::Release>(active_flag);
        record.record_kind.store<libk::MemoryOrder::Relaxed>(
            static_cast<u32>(kind));
        record.expectation.store<libk::MemoryOrder::Relaxed>(
            static_cast<u32>(expectation));
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
        record.started_at.store<libk::MemoryOrder::Release>(tick);
        record.last_activity_at.store<libk::MemoryOrder::Release>(tick);
        record.last_progress_at.store<libk::MemoryOrder::Release>(tick);
        return ObservationLease{ObservationKey::make(
            id_, static_cast<u16>(index), generation)};
    }
    mark_degraded();
    return {};
}

void ObservationShard::mark_degraded() noexcept {
    degraded_.store<libk::MemoryOrder::Release>(1);
    if (CpuDiagnosticsCore* const core = current_core(); core != nullptr) {
        static_cast<void>(core->status.flags.fetch_or<libk::MemoryOrder::Release>(
            DiagnosticStatus::ObservationFull));
    }
}

auto ObservationShard::key_at(usize index) const noexcept -> ObservationKey {
    if (index >= slot_count) {
        return {};
    }
    const u64 mask = bit(index);
    if ((allocated_.load<libk::MemoryOrder::Acquire>() & mask) == 0) {
        return {};
    }
    if ((records[index].lease_state.load<libk::MemoryOrder::Acquire>()
            & release_requested)
        != 0) {
        return {};
    }
    const u64 generation = generations[index].load<libk::MemoryOrder::Acquire>();
    return ObservationKey::make(id_, static_cast<u16>(index), generation);
}

auto ObservationShard::find(
    RecordKind kind,
    u64 subject_identity,
    u64 subject_generation) const noexcept -> ObservationKey {
    const u64 allocated = allocated_.load<libk::MemoryOrder::Acquire>();
    for (usize index = 0; index < slot_count; ++index) {
        const u64 mask = bit(index);
        if ((allocated & mask) == 0) {
            continue;
        }
        const ObservationRecord& record = records[index];
        if ((record.flags.load<libk::MemoryOrder::Acquire>() & active_flag)
                == 0
            || (record.lease_state.load<libk::MemoryOrder::Acquire>()
                    & release_requested)
                != 0
            || static_cast<RecordKind>(record.record_kind.load<
                   libk::MemoryOrder::Acquire>())
                != kind
            || record.subject_identity.load<libk::MemoryOrder::Acquire>()
                != subject_identity
            || record.subject_generation.load<libk::MemoryOrder::Acquire>()
                != subject_generation) {
            continue;
        }
        const u64 generation = record.generation.load<
            libk::MemoryOrder::Acquire>();
        return ObservationKey::make(
            id_, static_cast<u16>(index), generation);
    }
    return {};
}

void ObservationShard::release(ObservationKey key) noexcept {
    const usize index = key.slot();
    if (!key || key.shard() != id_ || index >= slot_count) {
        return;
    }
    const u64 mask = bit(index);
    if ((allocated_.load<libk::MemoryOrder::Acquire>() & mask) == 0) {
        return;
    }
    ObservationRecord& record = records[index];
    if (record.generation.load<libk::MemoryOrder::Acquire>()
        != key.generation()) {
        return;
    }
    const u64 tag = lease_generation(key.generation());
    u64 state = record.lease_state.load<libk::MemoryOrder::Acquire>();
    for (;;) {
        if ((state & lease_generation_mask) != tag
            || (state & release_requested) != 0) {
            return;
        }
        const u64 next = state | release_requested;
        if (record.lease_state.compare_exchange_weak<
                libk::MemoryOrder::AcqRel,
                libk::MemoryOrder::Acquire>(state, next)) {
            static_cast<void>(watched_.fetch_and<libk::MemoryOrder::Release>(
                ~mask));
            if ((state & pin_count_mask) != 0) {
                return;
            }
            break;
        }
    }
    record.flags.store<libk::MemoryOrder::Release>(0);
    static_cast<void>(allocated_.fetch_and<libk::MemoryOrder::Release>(~mask));
}

auto ObservationShard::write_metadata(
    ObservationKey key,
    u32 phase,
    WaitKind wait,
    NodeRef driver,
    NodeRef blocker,
    SourceSite site) noexcept -> bool {
    ObservationRecord* record{};
    if (!pin(key, record)) {
        return false;
    }
    increment_sat(record->activity_epoch, 1);
    record->last_activity_at.store<libk::MemoryOrder::Release>(now());
    u64 odd{};
    if (!AtomicSnapshotWriter::begin(record->sequence, odd)) {
        mark_degraded();
        unpin(key, *record);
        return false;
    }
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
    AtomicSnapshotWriter::end(record->sequence, odd);
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
    increment_sat(record->activity_epoch, 1);
    record->last_activity_at.store<libk::MemoryOrder::Release>(now());
    u64 odd{};
    if (!AtomicSnapshotWriter::begin(record->sequence, odd)) {
        mark_degraded();
        unpin(key, *record);
        return false;
    }
    record->waiter_key.store<libk::MemoryOrder::Relaxed>(wait.raw);
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

void ObservationShard::set_watched(
    ObservationKey key,
    bool watched) noexcept {
    ObservationRecord* record{};
    if (!pin(key, record)) {
        return;
    }
    const usize index = key.slot();
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
    SourceSite site) noexcept -> bool {
    ObservationRecord* record{};
    if (!pin(key, record)) {
        return false;
    }
    increment_sat(record->activity_epoch, 1);
    record->last_activity_at.store<libk::MemoryOrder::Release>(now());
    u64 odd{};
    if (!AtomicSnapshotWriter::begin(record->sequence, odd)) {
        mark_degraded();
        unpin(key, *record);
        return false;
    }
    record->phase.store<libk::MemoryOrder::Relaxed>(phase);
    record->site_file.store<libk::MemoryOrder::Relaxed>(
        reinterpret_cast<usize>(site.file));
    record->site_function.store<libk::MemoryOrder::Relaxed>(
        reinterpret_cast<usize>(site.function));
    record->site_line.store<libk::MemoryOrder::Relaxed>(site.line);
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
    u64 previous = record->semantic_stamp.load<libk::MemoryOrder::Acquire>();
    if (!force) {
        for (;;) {
            if (previous == semantic_stamp) {
                unpin(key, *record);
                return false;
            }
            if (record->semantic_stamp.compare_exchange_weak<
                    libk::MemoryOrder::AcqRel,
                    libk::MemoryOrder::Acquire>(previous, semantic_stamp)) {
                break;
            }
        }
    } else {
        record->semantic_stamp.store<libk::MemoryOrder::Release>(
            semantic_stamp);
    }
    increment_sat(record->progress_epoch, 1);
    record->last_progress_at.store<libk::MemoryOrder::Release>(now());
    unpin(key, *record);
    return true;
}

void ObservationShard::update_activity(
    ObservationKey key,
    u64 delta) noexcept {
    ObservationRecord* record{};
    if (delta == 0 || !pin(key, record)) {
        return;
    }
    increment_sat(record->activity_epoch, delta);
    record->last_activity_at.store<libk::MemoryOrder::Release>(now());
    unpin(key, *record);
}

void ObservationShard::advance(
    ObservationKey key,
    u64 delta) noexcept {
    ObservationRecord* record{};
    if (delta == 0 || !pin(key, record)) {
        return;
    }
    const u64 tick = now();
    increment_sat(record->activity_epoch, delta);
    increment_sat(record->progress_epoch, delta);
    record->last_activity_at.store<libk::MemoryOrder::Release>(tick);
    record->last_progress_at.store<libk::MemoryOrder::Release>(tick);
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
        value.generation = record->generation.load<libk::MemoryOrder::Relaxed>();
        value.flags = record->flags.load<libk::MemoryOrder::Relaxed>();
        value.activity_epoch = record->activity_epoch.load<libk::MemoryOrder::Relaxed>();
        value.progress_epoch = record->progress_epoch.load<libk::MemoryOrder::Relaxed>();
        value.started_at = record->started_at.load<libk::MemoryOrder::Relaxed>();
        value.last_activity_at = record->last_activity_at.load<libk::MemoryOrder::Relaxed>();
        value.last_progress_at = record->last_progress_at.load<libk::MemoryOrder::Relaxed>();
        value.record_kind = static_cast<RecordKind>(
            record->record_kind.load<libk::MemoryOrder::Relaxed>());
        value.phase = record->phase.load<libk::MemoryOrder::Relaxed>();
        value.wait_kind = static_cast<WaitKind>(
            record->wait_kind.load<libk::MemoryOrder::Relaxed>());
        value.expectation = static_cast<Expectation>(
            record->expectation.load<libk::MemoryOrder::Relaxed>());
        value.subject_identity = record->subject_identity.load<libk::MemoryOrder::Relaxed>();
        value.subject_generation = record->subject_generation.load<libk::MemoryOrder::Relaxed>();
        value.parent_key = ObservationKey{
            record->parent_key.load<libk::MemoryOrder::Relaxed>()};
        value.waiter_key = ObservationKey{
            record->waiter_key.load<libk::MemoryOrder::Relaxed>()};
        value.driver = read_node(
            record->driver_key.load<libk::MemoryOrder::Relaxed>(),
            record->driver_kind.load<libk::MemoryOrder::Relaxed>(),
            record->driver_generation.load<libk::MemoryOrder::Relaxed>());
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
            && value.generation == key.generation()
            && (value.flags & active_flag) != 0) {
            result = value;
            unpin(key, *record);
            return true;
        }
    }
    unpin(key, *record);
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
    increment_sat(record->activity_epoch, 1);
    record->last_activity_at.store<libk::MemoryOrder::Release>(tick);
    u64 odd{};
    if (AtomicSnapshotWriter::begin(record->sequence, odd)) {
        record->phase.store<libk::MemoryOrder::Relaxed>(terminal_phase);
        record->wait_kind.store<libk::MemoryOrder::Relaxed>(
            static_cast<u32>(WaitKind::None));
        record->waiter_key.store<libk::MemoryOrder::Relaxed>(0);
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
        AtomicSnapshotWriter::end(record->sequence, odd);
    } else {
        mark_degraded();
    }
    record->detail[0].store<libk::MemoryOrder::Release>(result);
    record->semantic_stamp.store<libk::MemoryOrder::Release>(result);
    increment_sat(record->progress_epoch, 1);
    record->last_progress_at.store<libk::MemoryOrder::Release>(tick);
    unpin(key, *record);
    profile_finish(kind, started != 0 ? elapsed(tick, started) : 0);
    release(key);
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
    CpuRuntime* const runtime = registry->runtime(key_.shard());
    const CpuDescriptor* const descriptor = registry->descriptor(key_.shard());
    if (runtime == nullptr || descriptor == nullptr
        || !descriptor->runtime_alive()
        || runtime->diagnostics == nullptr
        || runtime->diagnostics->concurrency.observations == nullptr) {
#if MYOS_BUILTIN_TESTS
        return resolve_standalone();
#else
        return false;
#endif
    }
    shard = runtime->diagnostics->concurrency.observations;
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

auto ObservationLease::find(
    RecordKind kind,
    u64 subject_identity,
    u64 subject_generation) noexcept -> ObservationLease {
#if MYOS_CONCURRENCY_DIAG >= 1
#if MYOS_BUILTIN_TESTS
    const auto find_standalone = [&]() noexcept -> ObservationLease {
        for (usize index = 0; index < 256; ++index) {
            ObservationShard* const shard = standalone_shards[index].load<
                libk::MemoryOrder::Acquire>();
            if (shard == nullptr) {
                continue;
            }
            const ObservationKey key = shard->find(
                kind, subject_identity, subject_generation);
            if (key) {
                return borrow(key);
            }
        }
        return {};
    };
#endif
    void* const owner = arch::current_cpu_owner();
    if (owner == nullptr) {
#if MYOS_BUILTIN_TESTS
        return find_standalone();
#else
        return {};
#endif
    }
    auto& cpu = *static_cast<CpuLocal*>(owner);
    if (cpu.runtime_ == nullptr || cpu.runtime_->owner_registry == nullptr) {
#if MYOS_BUILTIN_TESTS
        return find_standalone();
#else
        return {};
#endif
    }
    CpuRegistry* const registry = cpu.runtime_->owner_registry;
    for (usize index = 0; index < registry->count(); ++index) {
        const CpuId id{index};
        const CpuDescriptor* const descriptor = registry->descriptor(id);
        CpuRuntime* const runtime = registry->runtime(id);
        if (descriptor == nullptr || runtime == nullptr
            || !descriptor->runtime_alive()
            || runtime->diagnostics == nullptr
            || runtime->diagnostics->concurrency.observations == nullptr) {
            continue;
        }
        const ObservationKey key = runtime->diagnostics->concurrency.observations
            ->find(kind, subject_identity, subject_generation);
        if (key) {
            return borrow(key);
        }
    }
#if MYOS_BUILTIN_TESTS
    return find_standalone();
#else
    return {};
#endif
#else
    static_cast<void>(kind);
    static_cast<void>(subject_identity);
    static_cast<void>(subject_generation);
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
        key_, phase, wait, driver, blocker, site));
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
    attempt(phase, wait, driver, blocker, site);
#if MYOS_CONCURRENCY_DIAG >= 1
    ObservationShard* shard{};
    ObservationRecord* record{};
    if (resolve(shard, record)) {
        static_cast<void>(shard->update_progress(key_, semantic_stamp, false));
    }
#else
    static_cast<void>(semantic_stamp);
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
    static_cast<void>(shard->update_phase(key_, phase, site));
    static_cast<void>(shard->update_progress(key_, semantic_stamp, false));
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
    shard->update_activity(key_, 1);
    static_cast<void>(shard->update_progress(key_, semantic_stamp, false));
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
    if (index >= 4 || !resolve(shard, record)
        || !shard->pin(key_, record)) {
        return;
    }
    record->detail[index].store<libk::MemoryOrder::Release>(value);
    shard->unpin(key_, *record);
#else
    static_cast<void>(index);
    static_cast<void>(value);
#endif
}

void ObservationLease::detail_and(usize index, u64 mask) noexcept {
#if MYOS_CONCURRENCY_DIAG >= 1
    ObservationShard* shard{};
    ObservationRecord* record{};
    if (index >= 4 || !resolve(shard, record)
        || !shard->pin(key_, record)) {
        return;
    }
    static_cast<void>(record->detail[index].fetch_and<
        libk::MemoryOrder::AcqRel>(mask));
    shard->unpin(key_, *record);
#else
    static_cast<void>(index);
    static_cast<void>(mask);
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

void FlightRecorder::initialize(CpuId id) noexcept {
    id_ = id;
    head_.store<libk::MemoryOrder::Relaxed>(0);
    degraded_.store<libk::MemoryOrder::Relaxed>(0);
    wrapped_.store<libk::MemoryOrder::Relaxed>(0);
    for (auto& record : records) {
        record.sequence.store<libk::MemoryOrder::Relaxed>(0);
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
    FlightRecord& record = records[head % capacity];
    u64 odd{};
    if (!AtomicSnapshotWriter::begin(record.sequence, odd)) {
        degraded_.store<libk::MemoryOrder::Release>(1);
        return;
    }
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
    const FlightRecord& record = records[absolute % capacity];
    for (usize attempt = 0; attempt < 3; ++attempt) {
        const u64 first = AtomicSnapshotReader::begin(record.sequence);
        if ((first & 1U) != 0) {
            continue;
        }
        FlightRecordValue value{};
        value.sequence = first;
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
        if (AtomicSnapshotReader::valid(record.sequence, first)) {
            result = value;
            return true;
        }
    }
    return false;
}

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
        static_cast<void>(core->status.flags.fetch_or<
            libk::MemoryOrder::Release>(DiagnosticStatus::FlightWrapped));
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
    record(FlightDomain::Scheduler, FlightEvent::Dispatch,
        actor, context, cpu.raw, tick);
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
                    break;
                }
                target = CpuId{(target.raw + 1) % registry->count()};
            }
        }
    }
    if (shard == nullptr) {
        watcher->candidate.state.store<libk::MemoryOrder::Release>(
            static_cast<u32>(WatchdogCandidate::State::Clear));
        return;
    }
    const u64 live_hard = target_core->policy.critical_hard;
    const u64 live_soft = target_core->policy.critical_soft;
    const auto inspect_live = [&](libk::Atomic<u64>& since,
                                  DiagnosticStatus::Flag flag,
                                  u64 kind) noexcept {
        const u64 entered = since.load<libk::MemoryOrder::Acquire>();
        const bool stalled = live_hard != 0 && entered != 0
            && elapsed(tick, entered) >= live_hard;
        if (stalled) {
            const u32 previous = target_core->status.flags.fetch_or<
                libk::MemoryOrder::AcqRel>(flag);
            if ((previous & flag) == 0) {
                record(
                    FlightDomain::Watchdog,
                    FlightEvent::WatchdogConfirmed,
                    target_core->live.current_actor.load<
                        libk::MemoryOrder::Acquire>(),
                    target_core->live.current_obligation.load<
                        libk::MemoryOrder::Acquire>(),
                    kind,
                    elapsed(tick, entered),
                    target_core->live.context.load<
                        libk::MemoryOrder::Acquire>());
            }
        } else if (entered == 0
            || live_soft == 0
            || elapsed(tick, entered) < live_soft) {
            static_cast<void>(target_core->status.flags.fetch_and<
                libk::MemoryOrder::AcqRel>(~static_cast<u32>(flag)));
        }
    };
    inspect_live(
        target_core->live.irq_disabled_since,
        DiagnosticStatus::IrqStall,
        1);
    inspect_live(
        target_core->live.trap_entered_at,
        DiagnosticStatus::TrapStall,
        2);

    const u64 watched = shard->watched();
    ObservationKey key{};
    ObservationSnapshot snapshot{};
    u64 selected_age{};
    bool selected{};
    for (usize index = 0; index < ObservationShard::slot_count; ++index) {
        if ((watched & bit(index)) == 0) {
            continue;
        }
        const ObservationKey candidate_key = shard->key_at(index);
        ObservationSnapshot candidate_snapshot{};
        if (!candidate_key || !shard->snapshot(candidate_key, candidate_snapshot)) {
            continue;
        }
        if (!thresholds_for(target_core->policy, candidate_snapshot)) {
            continue;
        }
        const u64 candidate_age = elapsed(
            tick, candidate_snapshot.last_progress_at);
        if (!selected || candidate_age > selected_age) {
            key = candidate_key;
            snapshot = candidate_snapshot;
            selected_age = candidate_age;
            selected = true;
        }
    }
    const WatchdogThresholds thresholds = thresholds_for(
        target_core->policy, snapshot);
    if (!selected || !key || !thresholds) {
        watcher->candidate.state.store<libk::MemoryOrder::Release>(
            static_cast<u32>(WatchdogCandidate::State::Clear));
        return;
    }

    WatchdogCandidate& candidate = watcher->candidate;
    StallFingerprint previous{};
    const bool previous_valid = candidate.read(previous);
    const auto state = static_cast<WatchdogCandidate::State>(
        candidate.state.load<libk::MemoryOrder::Acquire>());
    const bool same = previous_valid && previous.key == key
        && previous.generation == snapshot.generation
        && previous.phase == snapshot.phase
        && previous.progress_epoch == snapshot.progress_epoch
        && previous.driver.kind == snapshot.driver.kind
        && previous.driver.identity == snapshot.driver.identity
        && previous.driver.generation == snapshot.driver.generation
        && previous.blocker.kind == snapshot.blocker.kind
        && previous.blocker.identity == snapshot.blocker.identity
        && previous.blocker.generation == snapshot.blocker.generation;

    const u64 age = elapsed(tick, snapshot.last_progress_at);
    if (!same) {
        candidate.publish(StallFingerprint{
            key,
            snapshot.generation,
            snapshot.phase,
            snapshot.progress_epoch,
            snapshot.activity_epoch,
            snapshot.driver,
            snapshot.blocker});
        candidate.first_seen.store<libk::MemoryOrder::Release>(0);
        candidate.state.store<libk::MemoryOrder::Release>(
            static_cast<u32>(WatchdogCandidate::State::Clear));
        return;
    }

    const bool active = snapshot.activity_epoch
        != previous.activity_epoch;
    previous.activity_epoch = snapshot.activity_epoch;
    candidate.publish(previous);
    if (age < thresholds.soft) {
        return;
    }
    if (state == WatchdogCandidate::State::Clear) {
        candidate.first_seen.store<libk::MemoryOrder::Release>(tick);
        candidate.state.store<libk::MemoryOrder::Release>(
            static_cast<u32>(WatchdogCandidate::State::Suspected));
        record(
            FlightDomain::Watchdog,
            FlightEvent::WatchdogSuspected,
            cpu.raw,
            key.raw,
            static_cast<u64>(snapshot.wait_kind),
            age);
    } else if (state == WatchdogCandidate::State::Suspected
        && age >= thresholds.hard) {
        const auto confirmed = active
            ? WatchdogCandidate::State::ConfirmedLivelock
            : WatchdogCandidate::State::Confirmed;
        candidate.state.store<libk::MemoryOrder::Release>(
            static_cast<u32>(confirmed));
        record(
            FlightDomain::Watchdog,
            active ? FlightEvent::WatchdogLivelock
                   : FlightEvent::WatchdogConfirmed,
            cpu.raw,
            key.raw,
            static_cast<u64>(confirmed),
            age);
    } else if (state == WatchdogCandidate::State::Confirmed && active) {
        candidate.state.store<libk::MemoryOrder::Release>(
            static_cast<u32>(WatchdogCandidate::State::ConfirmedLivelock));
        record(
            FlightDomain::Watchdog,
            FlightEvent::WatchdogLivelock,
            cpu.raw,
            key.raw,
            static_cast<u64>(WatchdogCandidate::State::ConfirmedLivelock),
            age);
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

void set_wait(
    WaitKind kind,
    ObservationKey wait,
    NodeRef subject,
    NodeRef driver,
    SourceSite site) noexcept {
#if MYOS_CONCURRENCY_DIAG >= 1
    if (CpuDiagnosticsCore* const core = current_core(); core != nullptr) {
        const u64 tick = now();
        core->live.current_wait.store<libk::MemoryOrder::Release>(wait.raw);
        core->live.current_subject.store<libk::MemoryOrder::Release>(
            subject.identity);
        core->live.current_subject_generation.store<
            libk::MemoryOrder::Release>(subject.generation);
        core->live.current_driver.store<libk::MemoryOrder::Release>(
            driver.identity);
        core->live.current_driver_generation.store<
            libk::MemoryOrder::Release>(driver.generation);
        core->live.current_obligation.store<libk::MemoryOrder::Release>(
            wait ? wait.raw : subject.identity);
        core->live.wait_kind.store<libk::MemoryOrder::Release>(
            static_cast<u32>(kind));
        core->live.wait_since.store<libk::MemoryOrder::Release>(tick);
        core->live.wait_site_file.store<libk::MemoryOrder::Release>(
            reinterpret_cast<usize>(site.file));
        core->live.wait_site_function.store<libk::MemoryOrder::Release>(
            reinterpret_cast<usize>(site.function));
        core->live.wait_site_line.store<libk::MemoryOrder::Release>(site.line);
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
}

void clear_wait() noexcept {
#if MYOS_CONCURRENCY_DIAG >= 1
    if (CpuDiagnosticsCore* const core = current_core(); core != nullptr) {
        core->live.current_wait.store<libk::MemoryOrder::Release>(0);
        core->live.current_subject.store<libk::MemoryOrder::Release>(0);
        core->live.current_subject_generation.store<
            libk::MemoryOrder::Release>(0);
        core->live.current_driver.store<libk::MemoryOrder::Release>(0);
        core->live.current_driver_generation.store<
            libk::MemoryOrder::Release>(0);
        core->live.current_obligation.store<libk::MemoryOrder::Release>(0);
        core->live.wait_kind.store<libk::MemoryOrder::Release>(
            static_cast<u32>(WaitKind::None));
        core->live.wait_since.store<libk::MemoryOrder::Release>(0);
        core->live.wait_site_file.store<libk::MemoryOrder::Release>(0);
        core->live.wait_site_function.store<libk::MemoryOrder::Release>(0);
        core->live.wait_site_line.store<libk::MemoryOrder::Release>(0);
    }
#endif
}

void mark_degraded(DiagnosticStatus::Flag flag) noexcept {
#if MYOS_CONCURRENCY_DIAG >= 1
    if (CpuDiagnosticsCore* const core = current_core(); core != nullptr) {
        static_cast<void>(core->status.flags.fetch_or<libk::MemoryOrder::Release>(flag));
        static_cast<void>(core->live.degraded.fetch_or<libk::MemoryOrder::Release>(flag));
    }
#else
    static_cast<void>(flag);
#endif
}

void dump_flight(CpuId id, const FlightRecorder& flight) noexcept {
#if MYOS_CONCURRENCY_DIAG >= 2
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
#else
    static_cast<void>(id);
    static_cast<void>(flight);
#endif
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
constexpr u32 execution_ready_phase = 1;
constexpr u32 execution_throttled_phase = 3;
constexpr u32 execution_blocked_phase = 4;
constexpr u32 operation_ready_phase = 3;

[[nodiscard]] auto terminal_class(
    const ObservationSnapshot& snapshot,
    bool livelock) noexcept -> StallClass {
    if (livelock) {
        return StallClass::Livelock;
    }
    if (snapshot.expectation == Expectation::ExternalUnbounded
        || is_external_wait(snapshot.wait_kind)) {
        return StallClass::ExternalWait;
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
    if (snapshot.record_kind == RecordKind::ServiceWork
        || snapshot.wait_kind == WaitKind::GrantWork) {
        return StallClass::LostServiceKick;
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
    ObservationKey root,
    WaitGraphScratch& scratch) noexcept -> bool {
#if MYOS_CONCURRENCY_DIAG >= 1
    scratch.classification = StallClass::None;
    scratch.count = 0;
    scratch.truncated = false;
    for (auto& key : scratch.path) {
        key = {};
    }
    if (!root) {
        scratch.classification = StallClass::Inconclusive;
        return false;
    }

    ObservationKey current = root;
    bool livelock = false;
    for (usize depth = 0; depth < graph_capacity; ++depth) {
        for (usize index = 0; index < scratch.count; ++index) {
            if (scratch.path[index] == current) {
                scratch.classification = StallClass::DeadlockCycle;
                return true;
            }
        }
        scratch.path[scratch.count++] = current;

        ObservationLease observation = ObservationLease::borrow(current);
        ObservationSnapshot snapshot{};
        if (!observation || !observation.snapshot(snapshot)) {
            scratch.classification = StallClass::Inconclusive;
            return false;
        }
        if (snapshot.activity_epoch > snapshot.progress_epoch
            && snapshot.activity_epoch > 1
            && snapshot.last_activity_at != snapshot.last_progress_at) {
            livelock = true;
        }

        // A blocked execution whose operation is already ready is the
        // observable half of the lost-wake proof.  The scheduler's wake
        // credit remains canonical in Binding; this check only correlates
        // the two stable diagnostic projections and never repairs state.
        if (snapshot.record_kind == RecordKind::ExecutionActor
            && snapshot.phase == execution_blocked_phase
            && snapshot.waiter_key) {
            ObservationLease wake = ObservationLease::borrow(
                snapshot.waiter_key);
            ObservationSnapshot operation{};
            if (!wake || !wake.snapshot(operation)) {
                scratch.classification = StallClass::Inconclusive;
                return false;
            }
            if (operation.record_kind == RecordKind::Operation
                && operation.phase == operation_ready_phase) {
                scratch.classification = StallClass::LostWake;
                return true;
            }
        }

        ObservationKey next = snapshot.waiter_key;
        if (!next && snapshot.driver.kind == NodeRef::Kind::Observation) {
            next = ObservationKey{snapshot.driver.identity};
            if (!next || next.generation() != snapshot.driver.generation) {
                scratch.classification = StallClass::Inconclusive;
                return false;
            }
        }
        if (!next) {
            scratch.classification = terminal_class(snapshot, livelock);
            return true;
        }
        current = next;
    }
    scratch.truncated = true;
    scratch.classification = StallClass::Truncated;
    return true;
#else
    static_cast<void>(root);
    static_cast<void>(scratch);
    return false;
#endif
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
      expectation_(expectation), site_(site) {
    if (*observation_) {
        observation_->attempt(0, kind_, driver_);
        observation_->watch(true);
        set_wait(kind_, owned_.key(), subject_, driver_, site_);
        linked_ = true;
    }
}

CpuWaitScope::CpuWaitScope(
    ObservationLease& observation,
    WaitKind kind,
    NodeRef subject,
    NodeRef driver,
    Expectation expectation,
    SourceSite site) noexcept
    : observation_(&observation), kind_(kind), subject_(subject),
      driver_(driver), expectation_(expectation), site_(site) {
    if (*observation_) {
        set_wait(kind_, observation_->key(), subject_, driver_, site_);
        linked_ = true;
    }
}

CpuWaitScope::~CpuWaitScope() noexcept {
    finish();
}

void CpuWaitScope::observe(u64 semantic_stamp) noexcept {
    if (observation_ == nullptr) {
        return;
    }
    if (!observed_ || semantic_stamp != last_stamp_) {
        observation_->observe(semantic_stamp);
        last_stamp_ = semantic_stamp;
        observed_ = true;
    } else {
        observation_->touch(site_);
    }
}

void CpuWaitScope::retarget(NodeRef driver, SourceSite site) noexcept {
    if (observation_ == nullptr || !*observation_) {
        return;
    }
    driver_ = driver;
    site_ = site;
    if (observation_ == &owned_) {
        observation_->attempt(0, kind_, driver_, {}, site_);
    }
    set_wait(kind_, observation_->key(), subject_, driver_, site_);
    linked_ = true;
}

void CpuWaitScope::finish() noexcept {
    if (observation_ == nullptr) {
        return;
    }
    if (linked_) {
        clear_wait();
        linked_ = false;
    }
    if (observation_ == &owned_) {
        owned_.finish(0);
    }
    observation_ = nullptr;
}

} // namespace kernel::diag::concurrency
