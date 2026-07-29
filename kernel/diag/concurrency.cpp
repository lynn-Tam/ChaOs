#include <diag/concurrency.hpp>

#include <arch/cpu.hpp>
#include <arch/time.hpp>
#include <cpu/cpu_local.hpp>
#include <cpu/cpu_registry.hpp>
#include <cpu/cpu_runtime.hpp>
#include <diag/console.hpp>
#include <diag/panic.hpp>
#include <execution/execution.hpp>
#include <libk/utility.hpp>

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
ObservationKey probe_fillers[ObservationShard::slot_count / 2]{};
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
[[nodiscard]] constexpr auto mix_hash(u64 hash, u64 value) noexcept -> u64 {
    hash ^= value + u64{0x9e3779b97f4a7c15}
        + (hash << 6) + (hash >> 2);
    return hash;
}

[[nodiscard]] auto snapshot_hash(
    const ObservationSnapshot& snapshot) noexcept -> u64 {
    u64 hash = u64{0x243f6a8885a308d3};
    hash = mix_hash(hash, snapshot.generation);
    hash = mix_hash(hash, snapshot.activity_epoch);
    hash = mix_hash(hash, snapshot.progress_epoch);
    hash = mix_hash(hash, static_cast<u64>(snapshot.record_kind));
    hash = mix_hash(hash, snapshot.phase);
    hash = mix_hash(hash, static_cast<u64>(snapshot.wait_kind));
    hash = mix_hash(hash, static_cast<u64>(snapshot.expectation));
    hash = mix_hash(hash, static_cast<u64>(snapshot.policy.kind));
    hash = mix_hash(hash, static_cast<u64>(snapshot.policy.expectation));
    hash = mix_hash(hash, snapshot.policy.driver.identity);
    hash = mix_hash(hash, static_cast<u64>(snapshot.policy.driver.kind));
    hash = mix_hash(hash, snapshot.policy.driver.generation);
    hash = mix_hash(hash, snapshot.policy.deadline);
    hash = mix_hash(hash, snapshot.policy.grace);
    hash = mix_hash(hash, static_cast<u64>(snapshot.policy.action));
    hash = mix_hash(hash, snapshot.subject_identity);
    hash = mix_hash(hash, snapshot.subject_generation);
    hash = mix_hash(hash, snapshot.parent_key.raw);
    hash = mix_hash(hash, snapshot.waiter_key.raw);
    hash = mix_hash(hash, snapshot.driver.identity);
    hash = mix_hash(hash, static_cast<u64>(snapshot.driver.kind));
    hash = mix_hash(hash, snapshot.driver.generation);
    hash = mix_hash(hash, snapshot.blocker.identity);
    hash = mix_hash(hash, static_cast<u64>(snapshot.blocker.kind));
    hash = mix_hash(hash, snapshot.blocker.generation);
    hash = mix_hash(hash, snapshot.semantic_stamp);
    for (const u64 value : snapshot.detail) {
        hash = mix_hash(hash, value);
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

constinit libk::Atomic<u64> stall_owner{
    libk::numeric_limits<u64>::max()};

[[nodiscard]] auto report_stall(
    CpuId watcher,
    CpuId target,
    ObservationKey key,
    WatchdogCandidate::State state,
    u64 age) noexcept -> bool {
    u64 token = mix_hash(u64{0x42}, target.raw);
    token = mix_hash(token, key.raw);
    token = mix_hash(token, static_cast<u64>(state));
    if (token == libk::numeric_limits<u64>::max()) {
        token = 0;
    }
    u64 expected = libk::numeric_limits<u64>::max();
    if (stall_owner.compare_exchange_strong<
            libk::MemoryOrder::AcqRel,
            libk::MemoryOrder::Acquire>(expected, token)) {
        diag::console::print<
            "[concurrency] watchdog confirmed cpu={} target={} key={:#x} "
            "state={} age={}\n">(
            watcher.raw,
            target.raw,
            key.raw,
            static_cast<u32>(state),
            age);
        return true;
    }
    return false;
}

[[nodiscard]] auto report_cpu_stall(
    CpuId watcher,
    CpuId target,
    u64 kind,
    u64 age) noexcept -> bool {
    // CPU-live roots have no ObservationKey. Keep their coordinator token in
    // a disjoint high-bit namespace so a stalled CPU cannot alias a slot key.
    u64 token = mix_hash(u64{0x7e}, target.raw);
    token = mix_hash(token, kind);
    if (token == libk::numeric_limits<u64>::max()) {
        token = 0;
    }
    u64 expected = libk::numeric_limits<u64>::max();
    if (stall_owner.compare_exchange_strong<
            libk::MemoryOrder::AcqRel,
            libk::MemoryOrder::Acquire>(expected, token)) {
        diag::console::print<
            "[concurrency] watchdog confirmed cpu-live cpu={} target={} "
            "kind={} age={}\n">(
            watcher.raw, target.raw, kind, age);
        return true;
    }
    return false;
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
    record.record_kind.store<libk::MemoryOrder::Relaxed>(0);
    record.phase.store<libk::MemoryOrder::Relaxed>(0);
    record.wait_kind.store<libk::MemoryOrder::Relaxed>(0);
    record.expectation.store<libk::MemoryOrder::Relaxed>(0);
    record.policy_kinds.store<libk::MemoryOrder::Relaxed>(0);
    record.policy_driver_key.store<libk::MemoryOrder::Relaxed>(0);
    record.policy_driver_generation.store<libk::MemoryOrder::Relaxed>(0);
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
    fingerprint_hash.store<libk::MemoryOrder::Relaxed>(value.state_hash);
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
        candidate.state_hash = fingerprint_hash.load<
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
        usize local{};
        ObservationPage* const page = page_for(index, local);
        if (page == nullptr) {
            continue;
        }
        const ObservationRecord& record = page->records[local];
        const u64 state = record.slot_state.load<libk::MemoryOrder::Acquire>();
        if (slot_kind(state) != SlotKind::Active
            || static_cast<RecordKind>(record.record_kind.load<
                   libk::MemoryOrder::Acquire>())
                != kind
            || record.subject_identity.load<libk::MemoryOrder::Acquire>()
                != subject_identity
            || record.subject_generation.load<libk::MemoryOrder::Acquire>()
                != subject_generation) {
            continue;
        }
        const u64 generation = slot_generation(state);
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
    record->last_activity_at.store<libk::MemoryOrder::Relaxed>(tick);
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
    const bool progressed = previous_phase != phase
        || (update_progress && previous_semantic != semantic_stamp);
    if (update_progress) {
        record->semantic_stamp.store<libk::MemoryOrder::Relaxed>(
            semantic_stamp);
    }
    if (progressed) {
        increment_sat(record->progress_epoch, 1);
        record->last_progress_at.store<libk::MemoryOrder::Relaxed>(tick);
    }
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
    u64 odd{};
    if (!AtomicSnapshotWriter::begin(record->sequence, odd)) {
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
    record->last_activity_at.store<libk::MemoryOrder::Relaxed>(tick);
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
    record->last_activity_at.store<libk::MemoryOrder::Relaxed>(tick);
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
        record->last_progress_at.store<libk::MemoryOrder::Relaxed>(tick);
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
    record->last_activity_at.store<libk::MemoryOrder::Relaxed>(tick);
    record->phase.store<libk::MemoryOrder::Relaxed>(phase);
    record->site_file.store<libk::MemoryOrder::Relaxed>(
        reinterpret_cast<usize>(site.file));
    record->site_function.store<libk::MemoryOrder::Relaxed>(
        reinterpret_cast<usize>(site.function));
    record->site_line.store<libk::MemoryOrder::Relaxed>(site.line);
    record->semantic_stamp.store<libk::MemoryOrder::Relaxed>(semantic_stamp);
    if (previous_phase != phase || previous_semantic != semantic_stamp) {
        increment_sat(record->progress_epoch, 1);
        record->last_progress_at.store<libk::MemoryOrder::Relaxed>(tick);
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
        record->last_progress_at.store<libk::MemoryOrder::Relaxed>(now());
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
    record->last_activity_at.store<libk::MemoryOrder::Relaxed>(tick);
    if (previous != semantic_stamp) {
        record->semantic_stamp.store<libk::MemoryOrder::Relaxed>(
            semantic_stamp);
        increment_sat(record->progress_epoch, 1);
        record->last_progress_at.store<libk::MemoryOrder::Relaxed>(tick);
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
    u64 odd{};
    if (!AtomicSnapshotWriter::begin(record->sequence, odd)) {
        mark_degraded(DiagnosticStatus::ObservationWriterCollision);
        degraded_.store<libk::MemoryOrder::Release>(1);
        unpin(key, *record);
        return;
    }
    if (!active(key, *record)) {
        AtomicSnapshotWriter::end(record->sequence, odd);
        unpin(key, *record);
        return;
    }
    increment_sat(record->activity_epoch, delta);
    record->last_activity_at.store<libk::MemoryOrder::Relaxed>(now());
    AtomicSnapshotWriter::end(record->sequence, odd);
    unpin(key, *record);
}

void ObservationShard::advance(
    ObservationKey key,
    u64 delta) noexcept {
    ObservationRecord* record{};
    if (delta == 0 || !pin(key, record)) {
        return;
    }
    u64 odd{};
    if (!AtomicSnapshotWriter::begin(record->sequence, odd)) {
        mark_degraded(DiagnosticStatus::ObservationWriterCollision);
        degraded_.store<libk::MemoryOrder::Release>(1);
        unpin(key, *record);
        return;
    }
    if (!active(key, *record)) {
        AtomicSnapshotWriter::end(record->sequence, odd);
        unpin(key, *record);
        return;
    }
    const u64 tick = now();
    increment_sat(record->activity_epoch, delta);
    increment_sat(record->progress_epoch, delta);
    record->last_activity_at.store<libk::MemoryOrder::Relaxed>(tick);
    record->last_progress_at.store<libk::MemoryOrder::Relaxed>(tick);
    AtomicSnapshotWriter::end(record->sequence, odd);
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
    u64 odd{};
    if (!AtomicSnapshotWriter::begin(record->sequence, odd)) {
        mark_degraded(DiagnosticStatus::ObservationWriterCollision);
        degraded_.store<libk::MemoryOrder::Release>(1);
        unpin(key, *record);
        return;
    }
    if (active(key, *record)) {
        record->detail[index].store<libk::MemoryOrder::Relaxed>(value);
    }
    AtomicSnapshotWriter::end(record->sequence, odd);
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
    u64 odd{};
    if (!AtomicSnapshotWriter::begin(record->sequence, odd)) {
        mark_degraded(DiagnosticStatus::ObservationWriterCollision);
        degraded_.store<libk::MemoryOrder::Release>(1);
        unpin(key, *record);
        return;
    }
    if (active(key, *record)) {
        static_cast<void>(record->detail[index].fetch_and<
            libk::MemoryOrder::Relaxed>(mask));
    }
    AtomicSnapshotWriter::end(record->sequence, odd);
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
            && active(key, *record)) {
            result = value;
            unpin(key, *record);
            return true;
        }
    }
    unpin(key, *record);
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
        record->last_activity_at.store<libk::MemoryOrder::Relaxed>(tick);
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
        record->detail[0].store<libk::MemoryOrder::Relaxed>(result);
        record->semantic_stamp.store<libk::MemoryOrder::Relaxed>(result);
        increment_sat(record->progress_epoch, 1);
        record->last_progress_at.store<libk::MemoryOrder::Relaxed>(tick);
        AtomicSnapshotWriter::end(record->sequence, odd);
        profile_finish(kind, started != 0 ? elapsed(tick, started) : 0);
    } else {
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
        ObservationShard* const shard = registry->observations(id);
        if (shard == nullptr) {
            continue;
        }
        const ObservationKey key = shard->find(
            kind, subject_identity, subject_generation);
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
        record.absolute_id.store<libk::MemoryOrder::Relaxed>(
            libk::numeric_limits<u64>::max());
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
    const FlightRecord& record = records[absolute % capacity];
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
    if (shard == nullptr) {
        watcher->candidate.state.store<libk::MemoryOrder::Release>(
            static_cast<u32>(WatchdogCandidate::State::Clear));
        return;
    }
    const u64 live_hard = target_core->policy.critical_hard;
    const u64 live_soft = target_core->policy.critical_soft;
    CpuLive::WaitSnapshot live_wait{};
    const bool live_wait_valid = target_core->live.top_wait(live_wait);
    const auto inspect_live = [&](libk::Atomic<u64>& since,
                                  DiagnosticStatus::Flag flag,
                                  u64 kind) noexcept {
        const u64 entered = since.load<libk::MemoryOrder::Acquire>();
        const bool stalled = live_hard != 0 && entered != 0
            && elapsed(tick, entered) >= live_hard;
        if (stalled) {
            const u32 previous = target_core->status().flags.fetch_or<
                libk::MemoryOrder::AcqRel>(flag);
            if ((previous & flag) == 0) {
                record(
                    FlightDomain::Watchdog,
                    FlightEvent::WatchdogConfirmed,
                    target_core->live.current_actor.load<
                        libk::MemoryOrder::Acquire>(),
                    live_wait_valid ? live_wait.obligation : 0,
                    kind,
                    elapsed(tick, entered),
                    target_core->live.context.load<
                        libk::MemoryOrder::Acquire>());
                static_cast<void>(target_core->status().flags.fetch_or<
                    libk::MemoryOrder::Release>(DiagnosticStatus::StallReported));
                static_cast<void>(report_cpu_stall(
                    cpu, target_cpu, kind, elapsed(tick, entered)));
            }
        } else if (entered == 0
            || live_soft == 0
            || elapsed(tick, entered) < live_soft) {
            static_cast<void>(target_core->status().flags.fetch_and<
                libk::MemoryOrder::AcqRel>(~static_cast<u32>(flag)));
        }
    };
    // The local watchdog runs from the timer trap with interrupts masked and
    // trap_depth/irq_depth elevated by that very callback.  It cannot be
    // evidence that this CPU is wedged: if local interrupts were genuinely
    // disabled, no later timer callback could observe the condition.  CPU-live
    // roots therefore require a peer; the local shard is still inspected for
    // ordinary observation obligations below.
    if (target_core != watcher) {
        inspect_live(
            target_core->live.irq_disabled_since,
            DiagnosticStatus::IrqStall,
            1);
        inspect_live(
            target_core->live.trap_entered_at,
            DiagnosticStatus::TrapStall,
            2);
    }

    const u64 watched = shard->watched();
    ObservationKey key{};
    ObservationSnapshot snapshot{};
    WatchdogThresholds selected_thresholds{};
    u64 selected_hash{};
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
        ObservationSnapshot wait_target{};
        const ObservationSnapshot* delegated{};
        if (candidate_snapshot.record_kind == RecordKind::ExecutionActor
            && candidate_snapshot.phase == static_cast<u32>(
                ExecutionState::Blocked)) {
            if (!candidate_snapshot.waiter_key) {
                static_cast<void>(target_core->status().flags.fetch_or<
                    libk::MemoryOrder::Release>(
                        DiagnosticStatus::PolicyMissing));
                continue;
            }
            ObservationLease lease = ObservationLease::borrow(
                candidate_snapshot.waiter_key);
            if (!lease || !lease.snapshot(wait_target)) {
                static_cast<void>(target_core->status().flags.fetch_or<
                    libk::MemoryOrder::Release>(
                        DiagnosticStatus::PolicyMissing));
                continue;
            }
            delegated = &wait_target;
        }
        const WatchdogThresholds candidate_thresholds = thresholds_for(
            target_core->policy, candidate_snapshot, delegated);
        if (!candidate_thresholds) {
            continue;
        }
        // An absolute business/refill deadline may still be in the future.
        // Unsigned elapsed() would wrap and let that legitimate future
        // obligation outrank every already-mature stall candidate.
        const u64 candidate_age = tick >= candidate_thresholds.anchor
            ? elapsed(tick, candidate_thresholds.anchor) : 0;
        if (!selected || candidate_age > selected_age) {
            key = candidate_key;
            snapshot = candidate_snapshot;
            selected_thresholds = candidate_thresholds;
            selected_hash = snapshot_hash(candidate_snapshot);
            if (delegated != nullptr) {
                selected_hash = mix_hash(
                    selected_hash, snapshot_hash(*delegated));
            }
            selected_age = candidate_age;
            selected = true;
        }
    }
    if (!selected || !key || !selected_thresholds) {
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
        && previous.state_hash == selected_hash
        && previous.driver.kind == snapshot.driver.kind
        && previous.driver.identity == snapshot.driver.identity
        && previous.driver.generation == snapshot.driver.generation
        && previous.blocker.kind == snapshot.blocker.kind
        && previous.blocker.identity == snapshot.blocker.identity
        && previous.blocker.generation == snapshot.blocker.generation;

    const u64 age = tick >= selected_thresholds.anchor
        ? elapsed(tick, selected_thresholds.anchor) : 0;
    if (!same) {
        candidate.publish(StallFingerprint{
            key,
            snapshot.generation,
            snapshot.phase,
            snapshot.progress_epoch,
            snapshot.activity_epoch,
            selected_hash,
            snapshot.driver,
            snapshot.blocker});
        candidate.first_seen.store<libk::MemoryOrder::Release>(0);
        candidate.state.store<libk::MemoryOrder::Release>(
            static_cast<u32>(WatchdogCandidate::State::Clear));
        return;
    }

    const u64 current_hash = selected_hash;
    const bool active = snapshot.activity_epoch != previous.activity_epoch
        && snapshot.progress_epoch == previous.progress_epoch;
    candidate.publish(StallFingerprint{
        key,
        snapshot.generation,
        snapshot.phase,
        snapshot.progress_epoch,
        snapshot.activity_epoch,
        current_hash,
        snapshot.driver,
        snapshot.blocker});
    if (tick < selected_thresholds.soft_at) {
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
        && tick >= selected_thresholds.hard_at) {
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
        if (report_stall(cpu, target_cpu, key, confirmed, age)) {
            static_cast<void>(target_core->status().flags.fetch_or<
                libk::MemoryOrder::Release>(DiagnosticStatus::StallReported));
        }
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
        if (report_stall(
                cpu,
                target_cpu,
                key,
                WatchdogCandidate::State::ConfirmedLivelock,
                age)) {
            static_cast<void>(target_core->status().flags.fetch_or<
                libk::MemoryOrder::Release>(DiagnosticStatus::StallReported));
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
    NodeRef root,
    WaitGraphScratch& scratch) noexcept -> bool {
#if MYOS_CONCURRENCY_DIAG >= 1
    scratch.classification = StallClass::None;
    scratch.count = 0;
    scratch.truncated = false;
    for (usize index = 0; index < graph_capacity; ++index) {
        scratch.set_node(index, {});
        scratch.path_meta[index] = 0;
        scratch.fingerprints[index] = 0;
    }
    if (!root) {
        scratch.classification = StallClass::Inconclusive;
        return false;
    }

    const auto read_cpu = [&](NodeRef node,
                              CpuLive::WaitSnapshot& result) noexcept -> bool {
        void* const owner = arch::current_cpu_owner();
        if (owner == nullptr) {
            return false;
        }
        auto& local = *static_cast<CpuLocal*>(owner);
        CpuRegistry* const registry = local.runtime_ == nullptr
            ? nullptr : local.runtime_->owner_registry;
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
        return runtime->diagnostics->concurrency.live.top_wait(result);
    };

    const auto read_node = [&](NodeRef node,
                               ObservationSnapshot& observation,
                               CpuLive::WaitSnapshot& cpu,
                               u64& fingerprint) noexcept -> bool {
        if (node.kind == NodeRef::Kind::Observation) {
            ObservationLease lease = ObservationLease::borrow(
                ObservationKey{node.identity});
            if (!lease || !lease.snapshot(observation)) {
                return false;
            }
            fingerprint = snapshot_hash(observation);
            return true;
        }
        if (node.kind == NodeRef::Kind::Cpu) {
            if (!read_cpu(node, cpu)) {
                return false;
            }
            fingerprint = u64{0x517cc1b727220a95};
            fingerprint = mix_hash(fingerprint, cpu.wait);
            fingerprint = mix_hash(fingerprint, cpu.subject.identity);
            fingerprint = mix_hash(fingerprint,
                static_cast<u64>(cpu.subject.kind));
            fingerprint = mix_hash(fingerprint, cpu.subject.generation);
            fingerprint = mix_hash(fingerprint, cpu.driver.identity);
            fingerprint = mix_hash(fingerprint,
                static_cast<u64>(cpu.driver.kind));
            fingerprint = mix_hash(fingerprint, cpu.driver.generation);
            fingerprint = mix_hash(fingerprint, cpu.obligation);
            fingerprint = mix_hash(fingerprint, cpu.since);
            fingerprint = mix_hash(fingerprint, static_cast<u64>(cpu.kind));
            return true;
        }
        if (node.kind == NodeRef::Kind::CpuSet) {
            ObservationLease lease = ObservationLease::borrow(
                ObservationKey{node.identity});
            if (!lease || !lease.snapshot(observation)
                || observation.generation != node.generation) {
                return false;
            }
            u64 pending{};
            CpuId selected{};
            bool found{};
            for (usize word = 0; word < 4 && !found; ++word) {
                pending = observation.detail[word];
                for (usize bit_index = 0; bit_index < 64; ++bit_index) {
                    if ((pending & (u64{1} << bit_index)) != 0) {
                        selected = CpuId{word * 64 + bit_index};
                        found = true;
                        break;
                    }
                }
            }
            if (!found || !read_cpu(NodeRef::cpu(selected), cpu)) {
                fingerprint = mix_hash(snapshot_hash(observation), 0);
                return true;
            }
            fingerprint = mix_hash(snapshot_hash(observation), selected.raw);
            fingerprint = mix_hash(fingerprint, cpu.wait);
            fingerprint = mix_hash(fingerprint, cpu.subject.identity);
            fingerprint = mix_hash(fingerprint, cpu.driver.identity);
            fingerprint = mix_hash(fingerprint, cpu.since);
            return true;
        }
        fingerprint = mix_hash(
            mix_hash(static_cast<u64>(node.kind), node.identity),
            node.generation);
        return true;
    };

    NodeRef current = root;
    bool done{};
    bool lost_wake{};
    ObservationSnapshot last_observation{};
    bool have_last_observation{};
    for (usize depth = 0; depth < graph_capacity; ++depth) {
        for (usize index = 0; index < scratch.count; ++index) {
            if (scratch.node(index) == current) {
                // Keep the operation in the evidence path, but let the
                // already-correlated wake publication win over the ordinary
                // back-edge classification.  The second snapshot pass below
                // still validates both nodes before reporting LostWake.
                scratch.classification = lost_wake
                    ? StallClass::LostWake
                    : StallClass::DeadlockCycle;
                done = true;
                break;
            }
        }
        if (done) {
            break;
        }
        ObservationSnapshot snapshot{};
        CpuLive::WaitSnapshot cpu{};
        u64 fingerprint{};
        if (!read_node(current, snapshot, cpu, fingerprint)) {
            scratch.classification = StallClass::Inconclusive;
            return false;
        }
        if (current.kind == NodeRef::Kind::Observation
            || current.kind == NodeRef::Kind::CpuSet) {
            last_observation = snapshot;
            have_last_observation = true;
        }
        if (scratch.count >= graph_capacity) {
            scratch.truncated = true;
            scratch.classification = StallClass::Truncated;
            done = true;
            break;
        }
        scratch.set_node(scratch.count, current);
        scratch.fingerprints[scratch.count] = fingerprint;
        ++scratch.count;

        NodeRef next{};
        if (current.kind == NodeRef::Kind::Observation) {
            // A blocked actor and a ReadyPublished operation are only a lost
            // wake when the scheduler projection has no queued/timer/credit
            // witness.  The canonical wake path remains authoritative.
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
                const bool scheduler_pending = (snapshot.detail[0] & 0xfU) != 0
                    || (snapshot.detail[2] & 0x7U) != 0;
                if (operation.record_kind == RecordKind::Operation
                    && operation.phase == static_cast<u32>(
                        OperationPhase::ReadyPublished)
                    && !scheduler_pending) {
                    scratch.classification = StallClass::LostWake;
                    lost_wake = true;
                }
            }
            next = snapshot.waiter_key
                ? NodeRef::observation(snapshot.waiter_key)
                : snapshot.driver;
            if (!next && snapshot.blocker.kind != NodeRef::Kind::None) {
                next = snapshot.blocker;
            }
        } else if (current.kind == NodeRef::Kind::Cpu
            || current.kind == NodeRef::Kind::CpuSet) {
            next = cpu.wait ? NodeRef::observation(
                ObservationKey{cpu.wait}) : cpu.driver;
        }
        if (done || !next) {
            if (!done && !lost_wake
                && (current.kind == NodeRef::Kind::Observation
                    || current.kind == NodeRef::Kind::CpuSet)) {
                scratch.classification = terminal_class(snapshot, false);
            } else if (!done && !lost_wake
                && current.kind == NodeRef::Kind::External) {
                // An external node is a terminal driver, not a classification
                // by itself. Preserve the obligation's policy and wait kind;
                // otherwise every finite Grant/Resource operation would be
                // mislabeled as an unbounded external wait merely because its
                // service actor is outside the bounded graph.
                scratch.classification = have_last_observation
                    ? terminal_class(last_observation, false)
                    : StallClass::ExternalWait;
            } else if (!done && !lost_wake
                && current.kind != NodeRef::Kind::Cpu) {
                scratch.classification = StallClass::ExternalWait;
            } else if (!done && !lost_wake) {
                scratch.classification = StallClass::UnclassifiedCpuStall;
            }
            done = true;
            break;
        }
        current = next;
    }

    if (!done) {
        scratch.truncated = true;
        scratch.classification = StallClass::Truncated;
    }

    // Re-read every node after the path is built.  A graph is evidence only
    // when all nodes survived one coherent diagnostic era.
    for (usize index = 0; index < scratch.count; ++index) {
        ObservationSnapshot observation{};
        CpuLive::WaitSnapshot cpu{};
        u64 fingerprint{};
        if (!read_node(
                scratch.node(index), observation, cpu, fingerprint)
            || fingerprint != scratch.fingerprints[index]) {
            scratch.classification = StallClass::Inconclusive;
            return false;
        }
    }
    return scratch.classification != StallClass::Inconclusive;
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

#if MYOS_CONCURRENCY_PROBE
void run_probe(u32 probe) noexcept {
    //Confirmatory experiment.
    // Exit condition: remove after an external kernel scenario runner can
    // deterministically pause a borrower at pin and writer boundaries.
    if (probe != 1) {
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
      expectation_(expectation), site_(site) {
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
    Expectation expectation,
    SourceSite site) noexcept
    : observation_(&observation), kind_(kind), subject_(subject),
      driver_(driver), expectation_(expectation), site_(site) {
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
