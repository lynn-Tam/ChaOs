#pragma once

// This header is the private representation boundary for concurrency
// diagnostics.  Common callers consume only <diag/concurrency.hpp>.
#include <diag/concurrency.hpp>
#include <libk/sync/atomic.hpp>

namespace kernel::diag::concurrency {

struct WaitGraphScratch final {
    StallClass classification{StallClass::None};
    EvidenceGrade evidence{EvidenceGrade::None};
    u8 count{};
    bool truncated{};
    u16 pending_total{};
    u8 pending_shown{};
    u16 pending_omitted{};
    u64 path[graph_capacity]{};
    u64 path_meta[graph_capacity]{};
    u64 fingerprints[graph_capacity]{};
    u8 parents[graph_capacity]{};
    EdgeKind edges[graph_capacity]{};

    [[nodiscard]] auto node(usize index) const noexcept -> NodeRef {
        if (index >= graph_capacity) {
            return {};
        }
        const u64 meta = path_meta[index];
        return NodeRef{static_cast<NodeRef::Kind>(meta & 0xffU),
            path[index], meta >> 8};
    }
    void set_node(usize index, NodeRef value) noexcept {
        if (index >= graph_capacity) {
            return;
        }
        path[index] = value.identity;
        path_meta[index] = static_cast<u64>(static_cast<u8>(value.kind))
            | (value.generation << 8);
    }
    [[nodiscard]] auto edge(usize index) const noexcept -> EdgeKind {
        return index < graph_capacity ? edges[index] : EdgeKind::None;
    }
};

struct ObservationRecord final {
    libk::Atomic<u64> slot_state{};
    libk::Atomic<u64> sequence{};
    libk::Atomic<u64> activity_epoch{};
    libk::Atomic<u64> progress_epoch{};
    libk::Atomic<u64> started_at{};
    libk::Atomic<u64> last_activity_at{};
    libk::Atomic<u64> last_progress_at{};
    libk::Atomic<u32> evidence{};
    libk::Atomic<u32> record_kind{};
    libk::Atomic<u32> phase{};
    libk::Atomic<u32> wait_kind{};
    libk::Atomic<u32> expectation{};
    libk::Atomic<u32> policy_kinds{};
    libk::Atomic<u64> policy_driver_key{};
    libk::Atomic<u64> policy_driver_generation{};
    libk::Atomic<u64> subject_identity{};
    libk::Atomic<u64> subject_generation{};
    libk::Atomic<u64> wait_target{};
    libk::Atomic<u64> driver_key{};
    libk::Atomic<u32> driver_kind{};
    libk::Atomic<u64> driver_generation{};
    libk::Atomic<u64> blocker_key{};
    libk::Atomic<u32> blocker_kind{};
    libk::Atomic<u64> blocker_generation{};
    libk::Atomic<u64> semantic_stamp{};
    libk::Atomic<u64> deadline{};
    libk::Atomic<u64> grace{};
    libk::Atomic<usize> site_file{};
    libk::Atomic<usize> site_function{};
    libk::Atomic<u32> site_line{};
    libk::Atomic<u64> detail[4]{};
};

struct ObservationPage final {
    static constexpr usize slot_count = 16;
    ObservationRecord records[slot_count]{};
};
static_assert(sizeof(ObservationPage) <= 4096);

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

class AtomicSnapshotWriter final {
public:
    [[nodiscard]] static auto begin(
        libk::Atomic<u64>& sequence, u64& odd) noexcept -> bool;
    static void end(libk::Atomic<u64>& sequence, u64 odd) noexcept;
};

class AtomicSnapshotReader final {
public:
    [[nodiscard]] static auto begin(
        const libk::Atomic<u64>& sequence) noexcept -> u64;
    [[nodiscard]] static auto valid(
        const libk::Atomic<u64>& sequence, u64 first) noexcept -> bool;
};

struct DiagnosticStatus final {
    using Flag = u32;
    static constexpr Flag None = DiagnosticFlag::None;
    static constexpr Flag SnapshotUnstable = DiagnosticFlag::SnapshotUnstable;
    static constexpr Flag FlightWrapped = DiagnosticFlag::FlightWrapped;
    static constexpr Flag StorageMissing = DiagnosticFlag::StorageMissing;
    static constexpr Flag ClockUnavailable = DiagnosticFlag::ClockUnavailable;
    static constexpr Flag IrqStall = DiagnosticFlag::IrqStall;
    static constexpr Flag TrapStall = DiagnosticFlag::TrapStall;
    static constexpr Flag ObservationCapacity = DiagnosticFlag::ObservationCapacity;
    static constexpr Flag ObservationGenerationExhausted =
        DiagnosticFlag::ObservationGenerationExhausted;
    static constexpr Flag ObservationWriterCollision =
        DiagnosticFlag::ObservationWriterCollision;
    static constexpr Flag FlightGap = DiagnosticFlag::FlightGap;
    static constexpr Flag WatchdogUnavailable = DiagnosticFlag::WatchdogUnavailable;
    static constexpr Flag StallReported = DiagnosticFlag::StallReported;
    static constexpr Flag WaitStackOverflow = DiagnosticFlag::WaitStackOverflow;
    static constexpr Flag ObservationLeaseCorrupt =
        DiagnosticFlag::ObservationLeaseCorrupt;
    static constexpr Flag RemoteShardUnavailable =
        DiagnosticFlag::RemoteShardUnavailable;
    static constexpr Flag PolicyMissing = DiagnosticFlag::PolicyMissing;
    static constexpr Flag ReportDropped = DiagnosticFlag::ReportDropped;

    libk::Atomic<u32> flags{};
};

class ReportNotifier final {
public:
    [[nodiscard]] auto install(ReportCallback callback) noexcept -> bool;
    void unbind() noexcept;
    [[nodiscard]] auto notify() noexcept -> bool;

private:
    static constexpr u64 bound_bit = 1;
    static constexpr u64 reader_bit = 2;
    static constexpr u64 reader_mask =
        libk::numeric_limits<u64>::max() & ~u64{1};

    ReportCallback callback_{};
    libk::Atomic<u64> state_{};
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

    void initialize(CpuId id, LatencyProfile* profile = nullptr,
        ObservationPage* const* storage = nullptr, usize page_count = 0,
        DiagnosticStatus* status = nullptr) noexcept;
    [[nodiscard]] auto reserve(RecordKind kind, u64 subject_identity,
        u64 subject_generation, Expectation expectation, SourceSite site) noexcept
        -> ObservationLease;
    [[nodiscard]] auto snapshot(ObservationKey key,
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
    void profile_current(u64 tick) const noexcept;

private:
    friend class ObservationLease;
    [[nodiscard]] auto valid(ObservationKey key) const noexcept
        -> ObservationRecord*;
    [[nodiscard]] auto page_for(usize index, usize& local) const noexcept
        -> ObservationPage*;
    [[nodiscard]] auto pin(ObservationKey key,
        ObservationRecord*& record) const noexcept -> bool;
    void unpin(ObservationKey key, ObservationRecord& record) const noexcept;
    [[nodiscard]] auto retire(ObservationKey key,
        ObservationRecord& record) noexcept -> bool;
    void reclaim(ObservationKey key, ObservationRecord& record) noexcept;
    [[nodiscard]] auto active(ObservationKey key,
        const ObservationRecord& record) const noexcept -> bool;
    void finish(ObservationKey key, u32 terminal_phase, u64 result,
        SourceSite site) noexcept;
    [[nodiscard]] auto write_metadata(ObservationKey key, u32 phase,
        WaitKind wait, NodeRef driver, NodeRef blocker, u64 semantic_stamp,
        bool update_progress, SourceSite site) noexcept -> bool;
    [[nodiscard]] auto write_batch(ObservationKey key,
        const ObservationBatch& update) noexcept -> bool;
    [[nodiscard]] auto write_wait(ObservationKey key, ObservationKey wait,
        WaitKind kind, NodeRef driver, SourceSite site) noexcept -> bool;
    [[nodiscard]] auto write_policy(ObservationKey key,
        OperationPolicy policy, SourceSite site) noexcept -> bool;
    [[nodiscard]] auto publish_operation(ObservationKey key,
        OperationPhase phase, NodeRef driver, NodeRef blocker,
        SourceSite site) noexcept -> bool;
    void write_deadline(ObservationKey key, u64 absolute, u64 grace,
        SourceSite site) noexcept;
    [[nodiscard]] auto update_progress(ObservationKey key,
        u64 semantic_stamp, bool force) noexcept -> bool;
    [[nodiscard]] auto observe(ObservationKey key,
        u64 semantic_stamp) noexcept -> bool;
    void update_activity(ObservationKey key, u64 delta) noexcept;
    void advance(ObservationKey key, u64 delta) noexcept;
    void write_detail(ObservationKey key, usize index, u64 value) noexcept;
    void and_detail(ObservationKey key, usize index, u64 mask) noexcept;
    [[nodiscard]] auto update_phase(ObservationKey key, u32 phase,
        u64 semantic_stamp, SourceSite site) noexcept -> bool;
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

struct FlightPage final {
    static constexpr usize capacity = 32;
    FlightRecord records[capacity]{};
};
static_assert(sizeof(FlightPage) <= 4096);

class FlightRecorder final {
public:
    static constexpr usize page_count = 4;
    static constexpr usize capacity = page_count * FlightPage::capacity;
    FlightRecorder() noexcept = default;
    FlightRecorder(const FlightRecorder&) = delete;
    auto operator=(const FlightRecorder&) -> FlightRecorder& = delete;
    void initialize(CpuId id,
        FlightPage* const (&storage)[page_count]) noexcept;
    void push(u64 tick, FlightDomain domain, FlightEvent event, u64 actor = 0,
        u64 subject = 0, u64 arg0 = 0, u64 arg1 = 0, u64 arg2 = 0,
        SourceSite site = SourceSite::current()) noexcept;
    [[nodiscard]] auto read(usize logical_index,
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
    FlightPage* pages_[page_count]{};
};
static_assert(sizeof(FlightRecorder) <= 4096);

struct CpuLive final {
    static constexpr usize wait_capacity = 4;
    enum class SnapshotMode : u8 { Relation, Strict };
    libk::Atomic<u64> dispatch_epoch{};
    libk::Atomic<u64> timer_epoch{};
    libk::Atomic<u64> trap_entered_at{};
    libk::Atomic<u64> irq_disabled_since{};
    libk::Atomic<u64> current_actor{};
    libk::Atomic<u32> wait_depth{};
    libk::Atomic<u64> wait_generation{};
    libk::Atomic<u64> wait_activity_epoch{};
    libk::Atomic<u64> wait_progress_epoch{};
    libk::Atomic<u64> wait_semantic_stamp{};
    libk::Atomic<u64> last_event_at{};
    libk::Atomic<u32> context{};
    libk::Atomic<u32> trap_depth{};
    libk::Atomic<u32> irq_depth{};
    libk::Atomic<u32> wait_overflow{};
    libk::Atomic<u32> degraded{};
    libk::Atomic<bool> interrupts_disabled{};
    struct WaitFrame final {
        libk::Atomic<u64> sequence{};
        libk::Atomic<u64> token{};
        libk::Atomic<u64> wait{};
        libk::Atomic<u64> subject_identity{};
        libk::Atomic<u64> subject_generation{};
        libk::Atomic<u64> driver_identity{};
        libk::Atomic<u64> driver_generation{};
        libk::Atomic<u64> obligation{};
        libk::Atomic<u64> since{};
        libk::Atomic<usize> site_file{};
        libk::Atomic<u32> site_line{};
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
    struct Snapshot final {
        u64 dispatch_epoch{};
        u64 timer_epoch{};
        u64 trap_entered_at{};
        u64 irq_disabled_since{};
        u64 current_actor{};
        u64 activity_epoch{};
        u64 progress_epoch{};
        u64 semantic_stamp{};
        u64 last_event_at{};
        u32 context{};
        u32 trap_depth{};
        u32 irq_depth{};
        u32 degraded{};
        bool interrupts_disabled{};
        bool has_wait{};
        WaitSnapshot wait{};
    };
    [[nodiscard]] auto top_wait(WaitSnapshot& result) const noexcept -> bool;
    [[nodiscard]] auto snapshot(Snapshot& result,
        SnapshotMode mode = SnapshotMode::Strict) const noexcept -> bool;
};

struct StallFingerprint final {
    NodeRef root{};
    u32 phase{};
    u64 progress_epoch{};
    u64 activity_epoch{};
    u64 relation_hash{};
    NodeRef driver{};
    NodeRef blocker{};
};

struct WatchdogCandidate final {
    enum class State : u32 { Clear, Suspected, Confirmed, ConfirmedLivelock };
    libk::Atomic<u32> state{};
    libk::Atomic<u64> fingerprint_sequence{};
    libk::Atomic<u64> root_identity{};
    libk::Atomic<u32> root_kind{};
    libk::Atomic<u64> root_generation{};
    libk::Atomic<u32> fingerprint_phase{};
    libk::Atomic<u64> fingerprint_progress{};
    libk::Atomic<u64> fingerprint_activity{};
    libk::Atomic<u64> relation_hash{};
    libk::Atomic<u64> driver_identity{};
    libk::Atomic<u32> driver_kind{};
    libk::Atomic<u64> driver_generation{};
    libk::Atomic<u64> blocker_identity{};
    libk::Atomic<u32> blocker_kind{};
    libk::Atomic<u64> blocker_generation{};
    libk::Atomic<u64> first_seen{};
    libk::Atomic<u32> active_intervals{};
    void publish(const StallFingerprint& value) noexcept;
    [[nodiscard]] auto read(StallFingerprint& value) const noexcept -> bool;
};

struct ReportRecord final {
    CpuId watcher{};
    CpuId target{};
    NodeRef root{};
    WatchdogCandidate::State state{};
    StallClass classification{StallClass::None};
    EvidenceGrade evidence{EvidenceGrade::None};
    u64 age{};
    WaitGraphScratch graph{};
};

class ReportQueue final {
public:
    static constexpr usize capacity = 1;
    [[nodiscard]] auto publish(const ReportRecord& value) noexcept -> bool;
    [[nodiscard]] auto consume(ReportRecord& value) noexcept -> bool;
    [[nodiscard]] auto pending() const noexcept -> bool;
    [[nodiscard]] auto lost() const noexcept -> u64 {
        return lost_.load<libk::MemoryOrder::Acquire>();
    }
private:
    libk::Atomic<u64> head_{};
    libk::Atomic<u64> tail_{};
    libk::Atomic<u64> lost_{};
    ReportRecord records_[capacity]{};
};

struct StallCoordinator final {
    static constexpr usize signature_capacity = 4;
    libk::Atomic<u64> owner{};
    libk::Atomic<usize> signature_cursor{};
    libk::Atomic<u64> signatures[signature_capacity]{};
};

struct ObservationStore final {
    LatencyProfile profile{};
    DiagnosticStatus status{};
    ObservationShard shard{};
};
static_assert(sizeof(ObservationStore) <= 4096);

struct CpuDiagnosticsCore final {
    static constexpr usize candidate_capacity = 4;
    CpuLive live{};
    WatchdogCandidate candidates[candidate_capacity]{};
    libk::Atomic<usize> candidate_cursor{};
    libk::Atomic<usize> scan_cursor{};
    StallCoordinator coordinator{};
    WaitGraphScratch graph{};
    ReportQueue reports{};
    WatchdogPolicy policy{};
    DiagnosticStatus fallback_status{};
    DiagnosticStatus* status_store{};
    LatencyProfile* profile{};
    FlightRecorder* flight{};
    ObservationShard* observations{};
    [[nodiscard]] auto status(this auto& self) noexcept -> decltype(auto) {
        return self.status_store == nullptr
            ? (self.fallback_status) : (*self.status_store);
    }
};

void dump_flight(CpuId id, const FlightRecorder& flight) noexcept;
[[nodiscard]] auto analyze(NodeRef root, WaitGraphScratch& scratch) noexcept
    -> bool;
[[nodiscard]] auto analyze(ObservationKey root,
    WaitGraphScratch& scratch) noexcept -> bool;

} // namespace kernel::diag::concurrency
