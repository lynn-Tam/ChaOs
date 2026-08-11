#pragma once

#include <core/types.hpp>
#include <cpu/topology.hpp>
#include <libk/array.hpp>
#include <libk/delegate.hpp>
#include <libk/limits.hpp>

namespace kernel {
class CpuRegistry;
class CpuRuntime;
namespace diag {
struct PanicSlot;
}
namespace mm {
class Pmm;
}
}

namespace kernel::diag::concurrency {

struct WatchdogPolicy;

// Diagnostic enablement is an out-of-line module boundary. Callers must not
// select a recorder policy in their own code-generation unit; the linked
// provider owns the selected level and returns the same typed value in real
// and off images.
enum class Level : u8 {
    Off,
    Snapshot,
    Trace,
    Watch,
    Profile,
};

extern const Level level;

[[nodiscard]] auto enabled(Level required) noexcept -> bool;

// Provisioning is owned by this module.  The off provider leaves the runtime
// owner pointers empty; real providers allocate only their private pages and
// publish typed borrows after construction.
[[nodiscard]] auto provision(
    CpuRuntime& runtime,
    CpuId id,
    mm::Pmm& pmm,
    const WatchdogPolicy& policy) noexcept -> bool;
[[nodiscard]] auto panic_slot(CpuRuntime& runtime) noexcept -> diag::PanicSlot*;

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
    static constexpr u64 cpu_generation = 1;

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
        return NodeRef{Kind::Cpu, id.raw, cpu_generation};
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
    /*luna change: expose MemoryExecutor service records, reason: diagnostics must project its retained queue distinctly*/
    MemoryWork,
    RemoteDelivery,
    ServiceWork,
    TrapContinuation,
    Count,
};

enum class WaitKind : u8 {
    None,
    SpinLock,
    CompletionPublication,
    CompletionDelivery,
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
    /*luna change: classify MemoryExecutor waits as bounded drain work, reason: stall policy must retain service visibility*/
    MemoryWork,
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

enum class StallAction : u8 {
    Record,
    Report,
};

// Immutable policy projected by one attached Completion generation.
// Completion owns the descriptor; observations retain only a bounded copy
// for cross-CPU analysis after the producer has left the call site.
struct OperationPolicy final {
    WaitKind kind{WaitKind::External};
    Expectation expectation{Expectation::ExternalUnbounded};
    NodeRef driver{};
    // Canonical owner of timeout publication.  This is distinct from the
    // ordinary operation producer: after the absolute deadline, the
    // dispatcher/timer path owns the obligation to publish completion.
    NodeRef deadline_driver{};
    u64 deadline{};
    u64 grace{};
    StallAction action{StallAction::Report};
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

// Projection of one canonical sched::RemoteRequest generation. The request
// owns the lease across both its linked and consumer-owned pending intervals;
// RemoteQueue owns only the queue and shared IPI transport state.
enum class RemotePhase : u32 {
    Posted = 1,
    NeedsKick = 2,
    InFlight = 3,
    Retry = 4,
    Taken = 5,
    Accepted = 6,
    Completed = 7,
    Cancelled = 8,
};

enum class ServicePhase : u32 {
    Queued = 1,
    WakeIssued = 2,
    Running = 3,
    Completed = 4,
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
    DeadlineDeliveryStall,
    DrainStall,
    Livelock,
    OrphanObligation,
    ExternalWait,
    UnclassifiedCpuStall,
    Inconclusive,
};

enum class EvidenceGrade : u8 {
    None,
    Suspected,
    Confirmed,
    Inconclusive,
    Degraded,
    External,
};

// The edge is part of the evidence, not a property inferred from the target
// node. A node may expose several relations in one coherent observation, and
// the first retained parent/edge is the stable explanation used by reports.
enum class EdgeKind : u8 {
    None,
    Wait,
    Driver,
    Blocker,
    Member,
};

inline constexpr usize graph_capacity = 12;

struct ObservationSnapshot final {
    u64 generation{};
    u64 activity_epoch{};
    u64 progress_epoch{};
    u64 started_at{};
    u64 last_activity_at{};
    u64 last_progress_at{};
    EvidenceGrade evidence{EvidenceGrade::None};
    RecordKind record_kind{};
    u32 phase{};
    WaitKind wait_kind{};
    Expectation expectation{};
    OperationPolicy policy{};
    u64 subject_identity{};
    u64 subject_generation{};
    ObservationKey wait_target{};
    NodeRef driver{};
    NodeRef blocker{};
    u64 semantic_stamp{};
    SourceSite site{};
    u64 detail[4]{};
};

// One projection publisher owns this metadata and may optionally publish all
// four details and the directory watch hint in the same bounded transaction.
// Multi-writer witness updates use detail()/detail_and()/advance() instead;
// detail_mask selects only the values captured by this epoch.
struct ObservationBatch final {
    u32 phase{};
    u64 semantic_stamp{};
    WaitKind wait{WaitKind::None};
    NodeRef driver{};
    NodeRef blocker{};
    SourceSite site{};
    u32 detail_mask{};
    u64 detail[4]{};
    // The semantic stamp is always published with the batch.  This flag only
    // controls whether the progress epoch/timestamp is advanced; callers that
    // already accounted independent work can keep the counters unchanged.
    bool update_progress{true};
    // A batch normally counts one metadata observation as activity.  A caller
    // that will immediately publish an independent witness (for example an
    // ObjectPool reference decrement) can leave activity accounting to that
    // witness so the canonical event is counted exactly once.
    bool update_activity{true};
    bool update_relation{true};
    bool update_deadline{};
    u64 deadline{};
    u64 grace{};
    bool update_watched{};
    bool watched{};
};

inline constexpr usize profile_bucket_count = 8;

struct ObservationRecord;
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
    void set_policy(
        OperationPolicy policy,
        SourceSite site = SourceSite::current()) noexcept;
    void publish(
        OperationPhase phase,
        NodeRef driver,
        NodeRef blocker = {},
        SourceSite site = SourceSite::current()) noexcept;
    void deadline(
        u64 absolute,
        u64 grace = 0,
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
    // Publish an independent progress witness. This deliberately does not
    // increment activity_epoch: progress itself resets the watchdog candidate,
    // while touch() and no-progress metadata remain activity-only witnesses.
    void advance(u64 delta = 1) noexcept;
    // Witness slots are diagnostic projections only.  They are deliberately
    // bounded and independently atomic so multi-CPU acknowledgements can
    // update a pending mask without taking a subsystem lock.
    void detail(usize index, u64 value) noexcept;
    void detail_and(usize index, u64 mask) noexcept;
    void publish(const ObservationBatch& update) noexcept;
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

    ObservationLease(
        ObservationKey key,
        bool owned = true) noexcept
        : key_(key), owned_(owned) {}
    [[nodiscard]] auto resolve(
        ObservationShard*& shard,
        ObservationRecord*& record) const noexcept -> bool;
    void reset() noexcept;

    ObservationKey key_{};
    bool owned_{};
};

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

struct DiagnosticFlag final {
    static constexpr u32 None = 0;
    static constexpr u32 SnapshotUnstable = 1U << 1;
    static constexpr u32 FlightWrapped = 1U << 2;
    static constexpr u32 StorageMissing = 1U << 3;
    static constexpr u32 ClockUnavailable = 1U << 4;
    static constexpr u32 IrqStall = 1U << 5;
    static constexpr u32 TrapStall = 1U << 6;
    static constexpr u32 ObservationCapacity = 1U << 7;
    static constexpr u32 ObservationGenerationExhausted = 1U << 8;
    static constexpr u32 ObservationWriterCollision = 1U << 9;
    static constexpr u32 FlightGap = 1U << 10;
    static constexpr u32 WatchdogUnavailable = 1U << 11;
    static constexpr u32 StallReported = 1U << 12;
    static constexpr u32 WaitStackOverflow = 1U << 13;
    static constexpr u32 ObservationLeaseCorrupt = 1U << 14;
    static constexpr u32 RemoteShardUnavailable = 1U << 15;
    static constexpr u32 PolicyMissing = 1U << 16;
    static constexpr u32 ReportDropped = 1U << 17;
};

struct WaitToken final {
    u64 raw{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return raw != 0;
    }
};

using ReportCallback = libk::delegate<bool() noexcept>;

[[nodiscard]] auto bind_report_notifier(
    ReportCallback notifier) noexcept -> bool;
void unbind_report_notifier() noexcept;
[[nodiscard]] auto drain_reports(CpuRegistry& registry) noexcept -> usize;

// Opaque owner lifetime and read-only evidence facade.  These functions keep
// provider pages, recorders, and report mailboxes out of common/public layout.
void destroy(CpuRuntime& runtime) noexcept;
[[nodiscard]] auto flight_head(const CpuRuntime& runtime) noexcept -> u64;
[[nodiscard]] auto flight_count(const CpuRuntime& runtime) noexcept -> usize;
[[nodiscard]] auto flight_read(
    const CpuRuntime& runtime,
    usize index,
    FlightRecordValue& result) noexcept -> bool;
[[nodiscard]] auto observation_snapshot(
    const CpuRuntime& runtime,
    ObservationKey key,
    ObservationSnapshot& result) noexcept -> bool;
[[nodiscard]] auto observation_key_at(
    const CpuRuntime& runtime,
    usize index) noexcept -> ObservationKey;
[[nodiscard]] auto observation_slot_count() noexcept -> usize;
[[nodiscard]] auto observation_watched(const CpuRuntime& runtime) noexcept
    -> u64;
[[nodiscard]] auto status_flags(const CpuRuntime& runtime) noexcept -> u32;
[[nodiscard]] auto report_pending(const CpuRuntime& runtime) noexcept -> bool;

// These functions are the only route from hot kernel paths into the current
// CPU diagnostic projection.  They are no-ops in the off profile.
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
[[nodiscard]] auto set_wait(
    WaitKind kind,
    ObservationKey wait,
    NodeRef subject,
    NodeRef driver,
    SourceSite site = SourceSite::current()) noexcept -> WaitToken;
void clear_wait(WaitToken token) noexcept;
[[nodiscard]] auto retarget_wait(
    WaitToken token,
    NodeRef driver,
    SourceSite site = SourceSite::current()) noexcept -> bool;
void observe_wait(WaitToken token, u64 semantic_stamp) noexcept;
[[nodiscard]] auto default_grace(Expectation expectation) noexcept -> u64;
void mark_degraded(u32 flag) noexcept;
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
    SourceSite site_{};
    u64 last_stamp_{};
    bool observed_{};
    WaitToken wait_token_{};
};

} // namespace kernel::diag::concurrency
