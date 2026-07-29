#pragma once

#include <core/types.hpp>
#include <cpu/topology.hpp>
#include <libk/array.hpp>
#include <libk/limits.hpp>
#include <libk/sync/atomic.hpp>

#ifndef MYOS_CONCURRENCY_DIAG
#define MYOS_CONCURRENCY_DIAG 0
#endif

namespace kernel::diag::concurrency {

struct DiagnosticStatus;

inline constexpr usize level = MYOS_CONCURRENCY_DIAG;
inline constexpr bool snapshot_enabled = level >= 1;
inline constexpr bool trace_enabled = level >= 2;
inline constexpr bool watch_enabled = level >= 3;
inline constexpr bool profile_enabled = level >= 4;

// A diagnostic key is an opaque identity, not a pointer.  The shard and slot
// locate a record only while its generation is active; consumers must validate
// all three fields before using a snapshot.  Generation exhaustion quarantines
// the slot instead of allowing an old key to name a new obligation.
struct ObservationKey final {
    u64 raw{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return raw != 0;
    }

    [[nodiscard]] static constexpr auto make(
        CpuId shard,
        u16 slot,
        u64 generation) noexcept -> ObservationKey {
        constexpr u64 generation_mask = (u64{1} << 40) - 1;
        if (shard.raw >= (u64{1} << 8) || slot == 0xffffU
            || generation == 0 || generation > generation_mask) {
            return {};
        }
        return ObservationKey{
            (generation << 24)
            | (static_cast<u64>(shard.raw) << 16)
            | (static_cast<u64>(slot) + 1)};
    }

    [[nodiscard]] constexpr auto shard() const noexcept -> CpuId {
        return CpuId{static_cast<usize>((raw >> 16) & 0xffU)};
    }

    [[nodiscard]] constexpr auto slot() const noexcept -> u16 {
        const u64 encoded = raw & 0xffffU;
        return encoded == 0 ? 0xffffU
                            : static_cast<u16>(encoded - 1);
    }

    [[nodiscard]] constexpr auto generation() const noexcept -> u64 {
        return raw >> 24;
    }

    friend constexpr auto operator==(
        ObservationKey, ObservationKey) noexcept -> bool = default;
};

struct NodeRef final {
    enum class Kind : u8 {
        None,
        Cpu,
        Observation,
        CpuSet,
        External,
    };

    Kind kind{};
    u64 identity{};
    u64 generation{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return kind != Kind::None;
    }

    [[nodiscard]] static constexpr auto cpu(CpuId id) noexcept -> NodeRef {
        return NodeRef{Kind::Cpu, id.raw, 1};
    }

    [[nodiscard]] static constexpr auto observation(
        ObservationKey key) noexcept -> NodeRef {
        return key ? NodeRef{Kind::Observation, key.raw, key.generation()}
                   : NodeRef{};
    }

    [[nodiscard]] static constexpr auto cpu_set(
        ObservationKey key) noexcept -> NodeRef {
        return key ? NodeRef{Kind::CpuSet, key.raw, key.generation()}
                   : NodeRef{};
    }

    [[nodiscard]] static constexpr auto external(
        u64 identity,
        u64 generation = 0) noexcept -> NodeRef {
        return identity == 0
            ? NodeRef{}
            : NodeRef{Kind::External, identity, generation};
    }

    friend constexpr auto operator==(NodeRef, NodeRef) noexcept -> bool = default;
};

struct SourceSite final {
    const char* file{};
    const char* function{};
    u32 line{};

    [[nodiscard]] static consteval auto current(
        const char* file = __builtin_FILE(),
        const char* function = __builtin_FUNCTION(),
        u32 line = __builtin_LINE()) noexcept -> SourceSite {
        return SourceSite{file, function, line};
    }
};

enum class RecordKind : u8 {
    ExecutionActor,
    ServiceActor,
    Operation,
    ExecutionStop,
    Shootdown,
    GrantRevoke,
    ResourceClose,
    ObjectRetire,
    VSpaceWork,
    RemoteDelivery,
    ServiceWork,
    Count,
};

enum class WaitKind : u8 {
    None,
    SpinLock,
    CompletionPublication,
    OperationCompletion,
    RemoteRequest,
    IpiDelivery,
    ShootdownAck,
    GrantWork,
    GrantOperations,
    GrantAttachments,
    ResourceReservations,
    ResourceConstructions,
    ResourceChildren,
    ResourceRefund,
    ObjectCleanup,
    ObjectReferences,
    ObjectPins,
    ObjectReclaim,
    VSpaceClaim,
    VSpaceShootdown,
    VSpaceAuthorityDrain,
    VSpaceActiveCpus,
    VSpaceWork,
    SchedulerReady,
    SchedulerRefill,
    SchedulerWake,
    SchedulerActivation,
    EndpointReply,
    ChannelSend,
    ChannelReceive,
    Notification,
    Tunnel,
    Pager,
    Irq,
    External,
    Unknown,
};

enum class Expectation : u8 {
    InternalFinite,
    DeadlineBound,
    ExternalUnbounded,
    SchedulerControlled,
    Idle,
    ObserveOnly,
};

// Operation delivery is the diagnostic projection of Completion's canonical
// delivery word.  The intermediate publication phase is intentional: a wake
// request may be issued before the producer publishes the operation as ready.
enum class OperationPhase : u32 {
    Attached = 1,
    Claimed = 2,
    WakeIssued = 3,
    ReadyPublished = 4,
    Finished = 5,
    Cancelled = 6,
};

enum class DriverKind : u8 {
    None,
    Actor,
    Observation,
    Cpu,
    CpuSet,
    Service,
    Deadline,
    External,
    Unknown,
};

enum class FlightDomain : u8 {
    Cpu,
    Trap,
    Lock,
    Scheduler,
    Operation,
    Remote,
    Shootdown,
    Grant,
    Resource,
    Object,
    VSpace,
    Ipc,
    Irq,
    Watchdog,
};

enum class FlightEvent : u8 {
    Timer,
    TrapEnter,
    TrapExit,
    IrqOff,
    IrqOn,
    LockAcquire,
    LockRelease,
    LockContended,
    Dispatch,
    Start,
    Yield,
    Ready,
    Block,
    Throttle,
    Refill,
    WakeRequested,
    WakeAccepted,
    Activation,
    Park,
    Exit,
    Reclaim,
    StopRequested,
    StopCompleted,
    OperationAttach,
    OperationClaimed,
    OperationReady,
    OperationFinish,
    OperationRelease,
    OperationCancel,
    RemotePost,
    RemoteTake,
    RemoteComplete,
    RemoteRetry,
    RemoteCoalesced,
    RemoteTransportClaim,
    RemoteTransportFailure,
    RemoteIpiSent,
    WatchdogSuspected,
    WatchdogConfirmed,
    WatchdogLivelock,
    ObservationDegraded,
};

// Bounded wait-graph output.  The scratch lives in the per-CPU diagnostics
// page, so panic reporting never grows the emergency stack or allocates.
enum class StallClass : u8 {
    None,
    DeadlockCycle,
    OpenOwnerStall,
    LostWake,
    TransportStall,
    LostServiceKick,
    RunnableStarvation,
    TimerStall,
    DrainStall,
    Livelock,
    OrphanObligation,
    ExternalWait,
    UnclassifiedCpuStall,
    Inconclusive,
    Truncated,
};

inline constexpr usize graph_capacity = 12;

struct WaitGraphScratch final {
    StallClass classification{StallClass::None};
    u8 count{};
    bool truncated{};
    // Keep the panic-page representation compact: the low byte of path_meta
    // stores NodeRef::Kind and the remaining 56 bits store generation.  All
    // diagnostic generations currently fit in that width (observation keys
    // use 40 bits), while identity remains lossless.
    u64 path[graph_capacity]{};
    u64 path_meta[graph_capacity]{};
    u64 fingerprints[graph_capacity]{};

    [[nodiscard]] auto node(usize index) const noexcept -> NodeRef {
        if (index >= graph_capacity) {
            return {};
        }
        const u64 meta = path_meta[index];
        return NodeRef{
            static_cast<NodeRef::Kind>(meta & 0xffU),
            path[index],
            meta >> 8};
    }
    void set_node(usize index, NodeRef value) noexcept {
        if (index >= graph_capacity) {
            return;
        }
        path[index] = value.identity;
        path_meta[index] = static_cast<u64>(
                static_cast<u8>(value.kind))
            | (value.generation << 8);
    }
};

struct ObservationRecord final {
    libk::Atomic<u64> sequence{};
    libk::Atomic<u64> generation{};
    libk::Atomic<u64> flags{};
    // Low bits count short-lived diagnostic pins; the high bit is a
    // non-blocking release request.  A slot cannot be reused while a pin is
    // present, so borrowed keys remain generation-safe across CPUs.
    libk::Atomic<u64> lease_state{};
    libk::Atomic<u64> activity_epoch{};
    libk::Atomic<u64> progress_epoch{};
    libk::Atomic<u64> started_at{};
    libk::Atomic<u64> last_activity_at{};
    libk::Atomic<u64> last_progress_at{};

    libk::Atomic<u32> record_kind{};
    libk::Atomic<u32> phase{};
    libk::Atomic<u32> wait_kind{};
    libk::Atomic<u32> expectation{};

    libk::Atomic<u64> subject_identity{};
    libk::Atomic<u64> subject_generation{};
    libk::Atomic<u64> parent_key{};
    libk::Atomic<u64> waiter_key{};
    libk::Atomic<u64> driver_key{};
    libk::Atomic<u32> driver_kind{};
    libk::Atomic<u64> driver_generation{};
    libk::Atomic<u64> blocker_key{};
    libk::Atomic<u32> blocker_kind{};
    libk::Atomic<u64> blocker_generation{};
    libk::Atomic<u64> semantic_stamp{};

    libk::Atomic<usize> site_file{};
    libk::Atomic<usize> site_function{};
    libk::Atomic<u32> site_line{};
    libk::Atomic<u64> detail[4]{};
};

// Records deliberately live in their own PMM pages.  A shard is only a
// directory and publication coordinator; it must remain cheap to embed in a
// CPU diagnostics page.  Sixteen records per page keeps the bitmaps and
// record array page-bounded without inflating Vproc/ObjectPool slots.
struct ObservationPage final {
    static constexpr usize slot_count = 16;

    libk::Atomic<u64> allocated{};
    libk::Atomic<u64> watched{};
    libk::Atomic<u32> degraded{};
    libk::Atomic<u64> generations[slot_count]{};
    ObservationRecord records[slot_count]{};
};

static_assert(sizeof(ObservationPage) <= 4096);

struct ObservationSnapshot final {
    u64 generation{};
    u64 flags{};
    u64 activity_epoch{};
    u64 progress_epoch{};
    u64 started_at{};
    u64 last_activity_at{};
    u64 last_progress_at{};
    RecordKind record_kind{};
    u32 phase{};
    WaitKind wait_kind{};
    Expectation expectation{};
    u64 subject_identity{};
    u64 subject_generation{};
    ObservationKey parent_key{};
    ObservationKey waiter_key{};
    NodeRef driver{};
    NodeRef blocker{};
    u64 semantic_stamp{};
    SourceSite site{};
    u64 detail[4]{};
};

inline constexpr usize profile_bucket_count = 8;

// Profile data is a diagnostic projection.  It is deliberately keyed by the
// same RecordKind used by observations, so it cannot become a second source
// of lifecycle state.  All counters saturate in the implementation.
struct LatencyStats final {
    libk::Atomic<u64> completed{};
    libk::Atomic<u64> total{};
    libk::Atomic<u64> max{};
    libk::Atomic<u64> current{};
    libk::Atomic<u64> buckets[profile_bucket_count]{};
};

struct LatencyProfile final {
    LatencyStats records[static_cast<usize>(RecordKind::Count)]{};

    void initialize() noexcept;
};

// Common sequence protocol used by both observation metadata and flight
// records.  Writers never wait: a concurrent writer marks the diagnostic
// projection degraded and leaves canonical kernel state untouched.
class AtomicSnapshotWriter final {
public:
    [[nodiscard]] static auto begin(
        libk::Atomic<u64>& sequence,
        u64& odd) noexcept -> bool;
    static void end(libk::Atomic<u64>& sequence, u64 odd) noexcept;
};

class AtomicSnapshotReader final {
public:
    [[nodiscard]] static auto begin(
        const libk::Atomic<u64>& sequence) noexcept -> u64;
    [[nodiscard]] static auto valid(
        const libk::Atomic<u64>& sequence,
        u64 first) noexcept -> bool;
};

class ObservationShard;

class ObservationLease final {
public:
    ObservationLease() noexcept = default;
    ObservationLease(const ObservationLease&) = delete;
    auto operator=(const ObservationLease&) -> ObservationLease& = delete;
    ObservationLease(ObservationLease&& other) noexcept;
    auto operator=(ObservationLease&& other) noexcept -> ObservationLease&;
    ~ObservationLease() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] auto key() const noexcept -> ObservationKey;

    [[nodiscard]] static auto reserve(
        RecordKind kind,
        u64 subject_identity = 0,
        u64 subject_generation = 0,
        Expectation expectation = Expectation::InternalFinite,
        SourceSite site = SourceSite::current()) noexcept -> ObservationLease;
    // Reserve on the execution's home CPU.  Binding publication can run on a
    // remote CPU, but the observation owner must remain with the home shard so
    // teardown and watchdog scans have one stable ownership path.
    [[nodiscard]] static auto reserve_on(
        CpuId cpu,
        RecordKind kind,
        u64 subject_identity = 0,
        u64 subject_generation = 0,
        Expectation expectation = Expectation::InternalFinite,
        SourceSite site = SourceSite::current()) noexcept -> ObservationLease;
    // Borrow an active key for a point update.  A borrowed lease never owns
    // the slot and therefore cannot accidentally release another actor's
    // observation when its temporary scope ends.
    [[nodiscard]] static auto borrow(ObservationKey key) noexcept
        -> ObservationLease;
    // Locate a live observation by its canonical subject identity.  The
    // result is a borrowed view; the subject owns the lifecycle, not this
    // temporary diagnostic handle.
    [[nodiscard]] static auto find(
        RecordKind kind,
        u64 subject_identity,
        u64 subject_generation) noexcept -> ObservationLease;
    // Transfer slot ownership to the caller as a key-only handle.  The slot
    // remains active until a later borrowed lease calls finish().
    [[nodiscard]] auto detach_key() noexcept -> ObservationKey;

    void attempt(
        u32 phase,
        WaitKind wait,
        NodeRef driver,
        NodeRef blocker = {},
        SourceSite site = SourceSite::current()) noexcept;
    void transition(
        u32 phase,
        u64 semantic_stamp,
        WaitKind wait,
        NodeRef driver,
        NodeRef blocker = {},
        SourceSite site = SourceSite::current()) noexcept;
    // Update only the phase and semantic progress.  Existing wait/driver
    // metadata remains intact, which lets an actor change scheduler state
    // while retaining the operation edge that explains a block.
    void phase(
        u32 phase,
        u64 semantic_stamp,
        SourceSite site = SourceSite::current()) noexcept;
    void observe(u64 semantic_stamp) noexcept;
    // Keep polling visible as activity without changing the semantic phase.
    void touch(SourceSite site = SourceSite::current()) noexcept;
    void advance(u64 delta = 1) noexcept;
    // Witness slots are diagnostic projections only.  They are deliberately
    // bounded and independently atomic so multi-CPU acknowledgements can
    // update a pending mask without taking a subsystem lock.
    void detail(usize index, u64 value) noexcept;
    void detail_and(usize index, u64 mask) noexcept;
    void link_wait(
        ObservationKey wait,
        WaitKind kind,
        NodeRef driver,
        SourceSite site = SourceSite::current()) noexcept;
    void clear_wait(SourceSite site = SourceSite::current()) noexcept;
    void watch(bool enabled) noexcept;
    void finish(
        u32 terminal_phase,
        u64 result = 0,
        SourceSite site = SourceSite::current()) noexcept;

    [[nodiscard]] auto snapshot(
        ObservationSnapshot& result) const noexcept -> bool;

private:
    friend class ObservationShard;

#if MYOS_CONCURRENCY_DIAG >= 1
    ObservationLease(
        ObservationKey key,
        bool owned = true) noexcept
        : key_(key), owned_(owned) {}
#else
    explicit ObservationLease(ObservationKey) noexcept {}
#endif

    [[nodiscard]] auto resolve(
        ObservationShard*& shard,
        ObservationRecord*& record) const noexcept -> bool;
    void reset() noexcept;

#if MYOS_CONCURRENCY_DIAG >= 1
    ObservationKey key_{};
    bool owned_{};
#endif
};

class ObservationShard final {
public:
    static constexpr usize pages = 4;
    static constexpr usize slots_per_page = ObservationPage::slot_count;
    static constexpr usize slot_count = pages * slots_per_page;

    ObservationShard() noexcept = default;
    ~ObservationShard() noexcept;
    ObservationShard(const ObservationShard&) = delete;
    auto operator=(const ObservationShard&) -> ObservationShard& = delete;

    void initialize(
        CpuId id,
        LatencyProfile* profile = nullptr,
        ObservationPage* const* storage = nullptr,
        usize page_count = 0,
        DiagnosticStatus* status = nullptr) noexcept;
    [[nodiscard]] auto reserve(
        RecordKind kind,
        u64 subject_identity,
        u64 subject_generation,
        Expectation expectation,
        SourceSite site) noexcept -> ObservationLease;
    [[nodiscard]] auto snapshot(
        ObservationKey key,
        ObservationSnapshot& result) const noexcept -> bool;
    void release(ObservationKey key) noexcept;
    void mark_degraded(u32 flag = 1U << 7) noexcept;

    [[nodiscard]] auto allocated() const noexcept -> u64 {
        return allocated_.load<libk::MemoryOrder::Acquire>();
    }
    [[nodiscard]] auto watched() const noexcept -> u64 {
        return watched_.load<libk::MemoryOrder::Acquire>();
    }
    [[nodiscard]] auto degraded() const noexcept -> bool {
        return degraded_.load<libk::MemoryOrder::Acquire>() != 0;
    }
    [[nodiscard]] auto key_at(usize index) const noexcept -> ObservationKey;
    [[nodiscard]] auto find(
        RecordKind kind,
        u64 subject_identity,
        u64 subject_generation) const noexcept -> ObservationKey;
    void profile_current(u64 tick) const noexcept;

private:
    friend class ObservationLease;

    [[nodiscard]] auto valid(ObservationKey key) const noexcept
        -> ObservationRecord*;
    [[nodiscard]] auto page_for(
        usize index,
        usize& local) const noexcept -> ObservationPage*;
    [[nodiscard]] auto pin(
        ObservationKey key,
        ObservationRecord*& record) const noexcept -> bool;
    void unpin(ObservationKey key, ObservationRecord& record) const noexcept;
    void finish(
        ObservationKey key,
        u32 terminal_phase,
        u64 result,
        SourceSite site) noexcept;
    [[nodiscard]] auto write_metadata(
        ObservationKey key,
        u32 phase,
        WaitKind wait,
        NodeRef driver,
        NodeRef blocker,
        u64 semantic_stamp,
        bool update_progress,
        SourceSite site) noexcept -> bool;
    [[nodiscard]] auto write_wait(
        ObservationKey key,
        ObservationKey wait,
        WaitKind kind,
        NodeRef driver,
        SourceSite site) noexcept -> bool;
    [[nodiscard]] auto update_progress(
        ObservationKey key,
        u64 semantic_stamp,
        bool force) noexcept -> bool;
    [[nodiscard]] auto observe(
        ObservationKey key,
        u64 semantic_stamp) noexcept -> bool;
    void update_activity(ObservationKey key, u64 delta) noexcept;
    void advance(ObservationKey key, u64 delta) noexcept;
    [[nodiscard]] auto update_phase(
        ObservationKey key,
        u32 phase,
        u64 semantic_stamp,
        SourceSite site) noexcept -> bool;
    void set_watched(ObservationKey key, bool watched) noexcept;
    void profile_finish(RecordKind kind, u64 duration) noexcept;

    CpuId id_{};
    LatencyProfile* profile_{};
    DiagnosticStatus* status_{};
    ObservationPage* storage_[pages]{};
    u8 page_count_{};
    libk::Atomic<u64> allocated_{};
    libk::Atomic<u64> watched_{};
    libk::Atomic<u32> degraded_{};
};

static_assert(sizeof(ObservationShard) <= 4096);

struct FlightRecordValue final {
    u64 sequence{};
    u64 absolute_id{};
    u64 tick{};
    FlightDomain domain{};
    FlightEvent event{};
    u64 actor{};
    u64 subject{};
    u64 arg0{};
    u64 arg1{};
    u64 arg2{};
    SourceSite site{};
};

struct FlightRecord final {
    libk::Atomic<u64> sequence{};
    libk::Atomic<u64> absolute_id{};
    libk::Atomic<u64> tick{};
    libk::Atomic<u32> domain{};
    libk::Atomic<u32> event{};
    libk::Atomic<u64> actor{};
    libk::Atomic<u64> subject{};
    libk::Atomic<u64> arg0{};
    libk::Atomic<u64> arg1{};
    libk::Atomic<u64> arg2{};
    libk::Atomic<usize> site_file{};
    libk::Atomic<usize> site_function{};
    libk::Atomic<u32> site_line{};
};

class FlightRecorder final {
public:
    static constexpr usize capacity = profile_enabled ? 40 : 32;

    FlightRecorder() noexcept = default;
    FlightRecorder(const FlightRecorder&) = delete;
    auto operator=(const FlightRecorder&) -> FlightRecorder& = delete;

    void initialize(CpuId id) noexcept;
    void push(
        u64 tick,
        FlightDomain domain,
        FlightEvent event,
        u64 actor = 0,
        u64 subject = 0,
        u64 arg0 = 0,
        u64 arg1 = 0,
        u64 arg2 = 0,
        SourceSite site = SourceSite::current()) noexcept;
    [[nodiscard]] auto read(
        usize logical_index,
        FlightRecordValue& result) const noexcept -> bool;
    [[nodiscard]] auto head() const noexcept -> u64 {
        return head_.load<libk::MemoryOrder::Acquire>();
    }
    [[nodiscard]] auto degraded() const noexcept -> bool {
        return degraded_.load<libk::MemoryOrder::Acquire>() != 0;
    }
    [[nodiscard]] auto wrapped() const noexcept -> bool {
        return wrapped_.load<libk::MemoryOrder::Acquire>() != 0;
    }

private:
    CpuId id_{};
    libk::Atomic<u64> head_{};
    libk::Atomic<u32> degraded_{};
    libk::Atomic<u32> wrapped_{};
    FlightRecord records[capacity]{};
};

static_assert(sizeof(FlightRecorder) <= 4096);

struct CpuLive final {
    static constexpr usize wait_capacity = 4;

    libk::Atomic<u64> dispatch_epoch{};
    libk::Atomic<u64> timer_epoch{};
    libk::Atomic<u64> trap_entered_at{};
    libk::Atomic<u64> irq_disabled_since{};
    libk::Atomic<u64> current_actor{};
    libk::Atomic<u32> wait_depth{};
    libk::Atomic<u64> wait_activity_epoch{};
    libk::Atomic<u64> wait_progress_epoch{};
    libk::Atomic<u64> wait_semantic_stamp{};
    libk::Atomic<u64> last_event_at{};
    libk::Atomic<u32> context{};
    libk::Atomic<u32> trap_depth{};
    libk::Atomic<u32> irq_depth{};
    // Scopes that arrive after the bounded frame stack is full still need a
    // matching close. Keep them as a count so clear_wait() cannot pop a real
    // outer frame on behalf of an overflowed inner scope.
    libk::Atomic<u32> wait_overflow{};
    libk::Atomic<u32> degraded{};
    libk::Atomic<bool> interrupts_disabled{};
    struct WaitFrame final {
        libk::Atomic<u64> sequence{};
        libk::Atomic<u64> wait{};
        libk::Atomic<u64> subject_identity{};
        libk::Atomic<u64> subject_generation{};
        libk::Atomic<u64> driver_identity{};
        libk::Atomic<u64> driver_generation{};
        libk::Atomic<u64> obligation{};
        libk::Atomic<u64> since{};
        libk::Atomic<usize> site_file{};
        libk::Atomic<u32> site_line{};
        // low byte subject kind, next byte driver kind, next byte wait kind
        libk::Atomic<u64> kinds{};
    };
    WaitFrame waits[wait_capacity]{};

    struct WaitSnapshot final {
        u64 wait{};
        NodeRef subject{};
        NodeRef driver{};
        u64 obligation{};
        u64 since{};
        WaitKind kind{WaitKind::None};
        SourceSite site{};
    };

    [[nodiscard]] auto top_wait(WaitSnapshot& result) const noexcept -> bool;
};

struct StallFingerprint final {
    ObservationKey key{};
    u64 generation{};
    u32 phase{};
    u64 progress_epoch{};
    u64 activity_epoch{};
    u64 state_hash{};
    NodeRef driver{};
    NodeRef blocker{};
};

struct WatchdogCandidate final {
    enum class State : u32 {
        Clear,
        Suspected,
        Confirmed,
        ConfirmedLivelock,
    };

    libk::Atomic<u32> state{};
    libk::Atomic<u64> fingerprint_sequence{};
    libk::Atomic<u64> fingerprint_key{};
    libk::Atomic<u64> fingerprint_generation{};
    libk::Atomic<u32> fingerprint_phase{};
    libk::Atomic<u64> fingerprint_progress{};
    libk::Atomic<u64> fingerprint_activity{};
    libk::Atomic<u64> fingerprint_hash{};
    libk::Atomic<u64> driver_identity{};
    libk::Atomic<u32> driver_kind{};
    libk::Atomic<u64> driver_generation{};
    libk::Atomic<u64> blocker_identity{};
    libk::Atomic<u32> blocker_kind{};
    libk::Atomic<u64> blocker_generation{};
    libk::Atomic<u64> first_seen{};

    void publish(const StallFingerprint& value) noexcept;
    [[nodiscard]] auto read(StallFingerprint& value) const noexcept -> bool;
};

// Thresholds are provisioned from the kernel clock. They are observation
// policy only; no functional path consults them.
struct WatchdogPolicy final {
    u64 critical_soft{};
    u64 critical_hard{};
    u64 transport_soft{};
    u64 transport_hard{};
    u64 service_soft{};
    u64 service_hard{};
    u64 scheduler_soft{};
    u64 scheduler_hard{};
};

struct DiagnosticStatus final {
    libk::Atomic<u32> flags{};
    enum Flag : u32 {
        None = 0,
        SnapshotUnstable = 1U << 1,
        FlightWrapped = 1U << 2,
        StorageMissing = 1U << 3,
        ClockUnavailable = 1U << 4,
        IrqStall = 1U << 5,
        TrapStall = 1U << 6,
        ObservationCapacity = 1U << 7,
        ObservationGenerationExhausted = 1U << 8,
        ObservationWriterCollision = 1U << 9,
        FlightGap = 1U << 10,
        WatchdogUnavailable = 1U << 11,
        StallReported = 1U << 12,
        WaitStackOverflow = 1U << 13,
        ObservationLeaseCorrupt = 1U << 14,
        RemoteShardUnavailable = 1U << 15,
    };
};

struct CpuDiagnosticsCore final {
    CpuLive live{};
    WatchdogCandidate candidate{};
    WaitGraphScratch graph{};
    WatchdogPolicy policy{};
    LatencyProfile profile{};
    DiagnosticStatus status{};
    FlightRecorder* flight{};
    ObservationShard* observations{};
};

// These functions are the only route from hot kernel paths into the current
// CPU diagnostic projection.  They are no-ops in the off profile.
[[nodiscard]] auto current_core() noexcept -> CpuDiagnosticsCore*;
[[nodiscard]] auto current_shard() noexcept -> ObservationShard*;
[[nodiscard]] auto reserve(
    RecordKind kind,
    u64 subject_identity = 0,
    u64 subject_generation = 0,
    Expectation expectation = Expectation::InternalFinite,
    SourceSite site = SourceSite::current()) noexcept -> ObservationLease;
[[nodiscard]] auto reserve_on(
    CpuId cpu,
    RecordKind kind,
    u64 subject_identity = 0,
    u64 subject_generation = 0,
    Expectation expectation = Expectation::InternalFinite,
    SourceSite site = SourceSite::current()) noexcept -> ObservationLease;

void record(
    FlightDomain domain,
    FlightEvent event,
    u64 actor = 0,
    u64 subject = 0,
    u64 arg0 = 0,
    u64 arg1 = 0,
    u64 arg2 = 0,
    SourceSite site = SourceSite::current()) noexcept;

void dispatch(CpuId cpu, u64 actor, u64 context, u64 tick) noexcept;
void timer(CpuId cpu, u64 tick) noexcept;
void trap_enter(u64 tick, u32 context) noexcept;
void trap_exit(u64 tick) noexcept;
void watchdog_tick(CpuId cpu, u64 tick) noexcept;
[[nodiscard]] auto irq_disabled(SourceSite site) noexcept -> u64;
void irq_restoring(u64 cookie) noexcept;
void set_wait(
    WaitKind kind,
    ObservationKey wait,
    NodeRef subject,
    NodeRef driver,
    SourceSite site = SourceSite::current()) noexcept;
void clear_wait() noexcept;
void observe_wait(u64 semantic_stamp) noexcept;
void mark_degraded(DiagnosticStatus::Flag flag) noexcept;
void dump_flight(CpuId id, const FlightRecorder& flight) noexcept;
[[nodiscard]] auto analyze(
    NodeRef root,
    WaitGraphScratch& scratch) noexcept -> bool;
[[nodiscard]] auto analyze(
    ObservationKey root,
    WaitGraphScratch& scratch) noexcept -> bool;
[[nodiscard]] auto stall_class_name(StallClass value) noexcept
    -> const char*;

class CpuWaitScope final {
public:
    CpuWaitScope(
        WaitKind kind,
        NodeRef subject,
        NodeRef driver,
        Expectation expectation,
        SourceSite site = SourceSite::current()) noexcept;
    CpuWaitScope(
        ObservationLease& observation,
        WaitKind kind,
        NodeRef subject,
        NodeRef driver,
        Expectation expectation,
        SourceSite site = SourceSite::current()) noexcept;
    CpuWaitScope(const CpuWaitScope&) = delete;
    auto operator=(const CpuWaitScope&) -> CpuWaitScope& = delete;
    ~CpuWaitScope() noexcept;

    void observe(u64 semantic_stamp) noexcept;
    void retarget(NodeRef driver, SourceSite site = SourceSite::current()) noexcept;
    void finish() noexcept;

private:
    ObservationLease owned_{};
    ObservationLease* observation_{};
    WaitKind kind_{};
    NodeRef subject_{};
    NodeRef driver_{};
    Expectation expectation_{};
    SourceSite site_{};
    u64 last_stamp_{};
    bool observed_{};
    bool linked_{};
};

} // namespace kernel::diag::concurrency
