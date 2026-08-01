#pragma once

#include <cpu/ipi_delivery.hpp>
#include <cpu/topology.hpp>
#include <diag/concurrency.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/limits.hpp>
#include <libk/noncopyable.hpp>
#include <libk/optional.hpp>
#include <libk/sync/atomic.hpp>
#include <sync/lock.hpp>

namespace kernel::sched {

enum class RemoteKind : u8 {
    Start,
    Wake,
    Activation,
    Stop,
};

enum class RemoteCancel : u8 {
    CanceledQueued,
    AlreadyClaimed,
    NotPending,
};

enum class RemotePost : u8 {
    Inserted,
    Coalesced,
};

struct RemotePostResult final {
    RemotePost disposition{RemotePost::Inserted};
    diag::concurrency::ObservationKey delivery{};
};

// A one-way projection of RemoteQueue.  The queue and request pending bits
// remain canonical under lock_; these atomics are only evidence for a
// watchdog or panic reader and may be stale or internally inconsistent.
struct RemoteSummary final {
    libk::Atomic<u64> queue_count{};
    libk::Atomic<u64> pending_count{};
    libk::Atomic<u32> oldest_kind{};
    libk::Atomic<u64> oldest_owner{};
    libk::Atomic<u64> post_epoch{};
    libk::Atomic<u64> take_epoch{};
    libk::Atomic<u64> complete_epoch{};
    libk::Atomic<u64> last_post{};
    libk::Atomic<u64> last_take{};
    libk::Atomic<u64> last_transport{};
    libk::Atomic<u32> delivery_state{};
    libk::Atomic<u64> delivery_generation{};
};

// Embedded in the state owner whose home CPU must commit a remote request.
// The pending bit spans queued and consumed work so producers can coalesce an
// edge without reusing an intrusive hook that the dispatcher still owns.
class RemoteRequest final : private libk::noncopyable_nonmovable {
public:
    RemoteRequest(RemoteKind kind, void* owner) noexcept;
    ~RemoteRequest() noexcept;

    [[nodiscard]] auto kind() const noexcept -> RemoteKind {
        return static_cast<RemoteKind>(owner_kind_ & kind_mask);
    }
    [[nodiscard]] auto owner() const noexcept -> void* {
        return reinterpret_cast<void*>(owner_kind_ & ~kind_mask);
    }
    // The low bit is the canonical pending state and the upper bits retain
    // the request generation across terminal completion.  Queue mutation is
    // serialized by RemoteQueue; the atomic read also lets destruction assert
    // that no consumer still owns the request.
    [[nodiscard]] auto pending() const noexcept -> bool {
        return (state_.load<libk::MemoryOrder::Acquire>() & 1U) != 0;
    }
    [[nodiscard]] auto generation() const noexcept -> u64;
    [[nodiscard]] auto delivery() const noexcept
        -> diag::concurrency::ObservationKey;
    // Best-effort diagnostic projection of the single cause recorded in the
    // delivery observation. The returned key is never a queue or scheduler
    // token; callers capture delivery() once while pending and may pass this
    // value only to another diagnostic publication.
    [[nodiscard]] auto diagnostic_cause(
        diag::concurrency::ObservationKey delivery) const noexcept
        -> diag::concurrency::ObservationKey;

private:
    friend class RemoteQueue;

    // Retain only the first concrete cause in the optional delivery
    // projection. Coalesced causes remain visible in flight records but must
    // not rewrite the provenance of the retained delivery edge.
    void retain_diagnostic_cause(
        diag::concurrency::ObservationKey delivery,
        diag::concurrency::ObservationKey cause) const noexcept;

    static constexpr usize kind_mask = 0x3;
    libk::IntrusiveListHook hook_{};
    usize owner_kind_{};
    // Canonical request lifecycle.  state = (generation << 1) | pending.
    // Keeping the generation after completion prevents a new post from
    // reusing the previous request identity even when diagnostics are off.
    libk::Atomic<u64> state_{};
    // Optional one-way diagnostic projections.  They are never consulted by
    // queue admission, coalescing, or terminal decisions.  Keep the raw key
    // atomic because a consumer may retain a request after take() while a
    // producer prepares its next generation under the queue lock.
    libk::Atomic<u64> delivery_{};
};

// One retained software-IPI edge for all scheduler work addressed to a CPU.
// The intrusive list owns queued work. take() transfers an unlinked request to
// the consumer while pending_ remains set; only complete() may release that
// consumer-owned state. IpiDelivery only tracks whether queued work still needs
// a transport kick.
class RemoteQueue final : private libk::noncopyable_nonmovable {
    using Queue = libk::IntrusiveList<
        RemoteRequest, &RemoteRequest::hook_>;

public:
    explicit RemoteQueue(CpuId home) noexcept : home_(home) {}

    [[nodiscard]] auto post(
        RemoteRequest& request,
        diag::concurrency::ObservationKey cause = {}) noexcept
        -> RemotePostResult;
    [[nodiscard]] auto claim_transport() noexcept
        -> libk::optional<kernel::IpiDelivery::Token>;
    void transport_failed(kernel::IpiDelivery::Token token) noexcept;
    [[nodiscard]] auto take() noexcept -> RemoteRequest*;
    void accepted(RemoteRequest& request, bool accepted) noexcept;
    void complete(RemoteRequest& request) noexcept;
    [[nodiscard]] auto cancel(RemoteRequest& request) noexcept
        -> RemoteCancel;
    [[nodiscard]] auto size() const noexcept -> usize;
    [[nodiscard]] auto summary() const noexcept -> const RemoteSummary& {
        return summary_;
    }

private:
    void publish_summary() noexcept;
    void publish(
        RemoteRequest& request,
        diag::concurrency::RemotePhase phase,
        diag::concurrency::WaitKind wait,
        u64 transport_generation = 0) noexcept;
    void publish_queued(
        diag::concurrency::RemotePhase phase,
        u64 transport_generation) noexcept;

    mutable kernel::sync::SpinLock<kernel::sync::LockClass::RemoteQueue>
        lock_{};
    Queue queue_{};
    kernel::IpiDelivery delivery_{};
    RemoteSummary summary_{};
    usize pending_count_{};
    CpuId home_{};
};

} // namespace kernel::sched
