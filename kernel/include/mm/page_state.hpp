#pragma once

#include <core/types.hpp>
#include <libk/expected.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/limits.hpp>
#include <libk/noncopyable.hpp>
#include <libk/sync/atomic.hpp>

namespace kernel::mm {

// Page materialization is a MemoryObject-owned protocol.  Pager transport
// records may refer to a page, but they never own this state.
enum class PageSlotState : u8 {
    Missing,
    Requesting,
    Requested,
    Filling,
    ResidentClean,
    ResidentDirty,
    WritebackQueued,
    WritebackPublishing,
    WritebackPublished,
    WritebackActive,
    WritebackCompleting,
    WritebackFailed,
    Evicting,
    Failed,
    Detaching,
    Released,
};

enum class PageStateError : u8 {
    InvalidTransition,
    StaleGeneration,
    InvalidRange,
    Busy,
    GenerationExhausted,
};

enum class PageWaitState : u8 {
    Detached,
    Attached,
    Publishing,
    Published,
    Released,
};

enum class PageWaitResult : u8 {
    Ready,
    Failed,
    Canceled,
    Stopped,
    OutOfMemory,
};

/*luna change: represent queued, publishing, and published transport phases explicitly, reason: transport slot zero is a valid Pager identity and cannot encode request ownership*/
enum class PageRequestState : u8 {
    Idle,
    Queued,
    Publishing,
    Published,
    Claimed,
    Ready,
    Failed,
};

struct PageRequest;
class WaitClaim;

/*luna change: describe finalize-before-callback relation lifetime, reason: host detaches storage before runnable delivery*/
// Continuation-owned relation storage. The owner embeds this node in fixed
// Wait/FaultSlot storage; PageRequest and PageReclaimer only index it and
// never allocate or free a relation record. A terminal claim is one-way and
// finalizes the relation before its callback, making slot reuse safe.
struct WaitRelation final {
    using Publish = void (*)(void*, PageWaitResult) noexcept;

    libk::IntrusiveListHook hook_{};
    union {
        PageRequest* request;
        u64 observed_progress;
    };
    void* owner{};
    Publish publish{};
    u64 generation{};
    libk::Atomic<u8> state_{static_cast<u8>(PageWaitState::Detached)};

    [[nodiscard]] auto attached() const noexcept -> bool {
        const auto state = static_cast<PageWaitState>(state_.load<
            libk::MemoryOrder::Acquire>());
        return state != PageWaitState::Detached;
    }
    [[nodiscard]] auto state() const noexcept -> PageWaitState {
        return static_cast<PageWaitState>(state_.load<
            libk::MemoryOrder::Acquire>());
    }
    // Called under the host lock. It removes the node from its intrusive
    // index, changes the atomic phase to Publishing, and returns one move-only
    // token. No owner callback runs while the host lock is held.
    [[nodiscard]] auto claim(
        u64 expected_generation,
        PageWaitResult result,
        WaitClaim& claim,
        PageRequest* request = nullptr) noexcept -> bool;
};

class WaitClaim final : private libk::noncopyable {
    enum class Phase : u8 {
        Claimed,
        Published,
        Released,
    };

public:
    WaitClaim() noexcept = default;
    WaitClaim(WaitClaim&& other) noexcept;
    auto operator=(WaitClaim&& other) noexcept -> WaitClaim&;
    ~WaitClaim() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept {
        /*luna change: keep finalized claims engaged after host unlink, reason: the callback snapshot remains valid until release/reset even though relation_ is detached*/
        return generation_ != 0 && publish_ != nullptr
            && (relation_ != nullptr || finalized_)
            && (phase_ == Phase::Claimed || phase_ == Phase::Published
                || phase_ == Phase::Released);
    }
    [[nodiscard]] auto relation() const noexcept -> WaitRelation* {
        return relation_;
    }
    [[nodiscard]] auto request() const noexcept -> PageRequest* {
        return request_;
    }
    [[nodiscard]] auto generation() const noexcept -> u64 {
        return generation_;
    }
    [[nodiscard]] auto publish() noexcept -> bool;
    [[nodiscard]] auto release() noexcept -> bool;
    // The owner calls reset only after host finalization, callback, and release.
    void reset() noexcept;

private:
    friend struct PageRequest;
    friend class PageReclaimer;
    friend struct WaitRelation;
    [[nodiscard]] auto finalize() noexcept -> bool;
    WaitClaim(
        WaitRelation& relation,
        u64 generation,
        void* owner,
        WaitRelation::Publish publish,
        PageWaitResult result,
        PageRequest* request) noexcept
        : relation_(&relation),
          generation_(generation),
          owner_(owner),
          publish_(publish),
          result_(result),
          request_(request) {}

    WaitRelation* relation_{};
    u64 generation_{};
    void* owner_{};
    WaitRelation::Publish publish_{};
    PageWaitResult result_{PageWaitResult::Canceled};
    PageRequest* request_{};
    bool finalized_{};
    Phase phase_{Phase::Claimed};
};

struct PageKey final {
    u64 generation{};
    usize index{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return generation != 0;
    }
    [[nodiscard]] friend constexpr auto operator==(
        PageKey, PageKey) noexcept -> bool = default;
};

struct WritebackKey final {
    PageKey page{};
    u64 generation{};
    u64 dirty_epoch{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return page && generation != 0 && dirty_epoch != 0;
    }
    [[nodiscard]] friend constexpr auto operator==(
        WritebackKey, WritebackKey) noexcept -> bool = default;
};

enum class WritebackAction : u8 {
    Publish,
    Complete,
    Fail,
};

enum class WritebackFailure : u8 {
    BackingUnavailable,
    Io,
};

struct WritebackTxn final {
    WritebackAction action{WritebackAction::Publish};
    WritebackKey key{};
    u64 delivery_generation{};
    u64 claim_generation{};
    WritebackFailure failure{WritebackFailure::BackingUnavailable};
};

struct PageRequest final {
    PageKey key{};
    usize first{};
    usize count{};
    u64 claim_generation{};
    /*luna change: keep PageRequest free of Pager identities, reason: semantic request state must not shadow transport membership*/
    PageRequestState state{PageRequestState::Idle};
    libk::IntrusiveList<WaitRelation, &WaitRelation::hook_>
        waiters{};
    u32 publishers{};

    [[nodiscard]] auto reset() noexcept -> bool;
    [[nodiscard]] auto begin_publish() noexcept -> bool;
    [[nodiscard]] auto publish() noexcept -> bool;
    [[nodiscard]] auto abort_publish() noexcept -> bool;
    [[nodiscard]] auto finish(PageWaitResult result) noexcept -> bool;
    [[nodiscard]] auto terminal_result() const noexcept -> PageWaitResult;
    [[nodiscard]] auto attach(
        WaitRelation& relation,
        void* owner,
        WaitRelation::Publish publish) noexcept -> bool;
    /*luna change: detach one exact waiter under the host lock, reason: fault cancellation must erase the relation without scanning backing nodes*/
    [[nodiscard]] auto detach(
        WaitRelation& relation,
        u64 expected_generation) noexcept -> bool;
    // Fixed-budget owner-lock phase; each WaitClaim carries the exact
    // continuation generation for the lock-free publish phase.
    [[nodiscard]] auto claim_waiters(
        WaitClaim* claims,
        usize capacity,
        PageWaitResult result) noexcept -> usize;
    // Called by the MemoryObject host lock after a token has published and
    // released. It is the only PageRequest-side relation reuse boundary.
    [[nodiscard]] auto finish_claim(WaitClaim& claim) noexcept -> bool;

    [[nodiscard]] constexpr auto contains(usize page) const noexcept -> bool {
        return page >= first && page - first < count;
    }
};

struct PageSlot final {
    struct WritebackRequest final {
        u64 generation{};
        u64 dirty_epoch{};
        u64 delivery_generation{};
        u64 claim_generation{};
        /*luna change: keep writeback transport identity with its generations, reason: Pager delivery metadata has one PageSlot owner*/
        u16 transport_slot{};
        WritebackFailure failure{WritebackFailure::BackingUnavailable};
        bool retained{};

        void reset() noexcept {
            const u64 generation_counter = generation;
            *this = {};
            generation = generation_counter;
        }
    };

    PageSlotState state{PageSlotState::Missing};
    u64 generation{};
    u64 content_epoch{};
    u64 dirty_epoch{};
    PageRequest request{};
    WritebackRequest writeback{};
    /*luna change: retain reclaim intent independently of writeback transport,
      reason: candidate policy survives queue, mapping and worker passes*/
    bool reclaim_intent{};

    [[nodiscard]] auto begin_request(
        PageKey key,
        usize first,
        usize count) noexcept
        -> libk::Expected<PageRequest*, PageStateError>;
    void cancel_request() noexcept;
    /*luna change: keep begin_fill as the sole page-in claim gate, reason: duplicate claim APIs would create parallel Requested-to-Filling transitions*/
    [[nodiscard]] auto begin_fill(u64 generation) noexcept
        -> libk::Expected<void, PageStateError>;
    [[nodiscard]] auto supply(u64 generation, u64 content_epoch) noexcept
        -> libk::Expected<void, PageStateError>;
    [[nodiscard]] auto fail(u64 generation) noexcept
        -> libk::Expected<void, PageStateError>;
    [[nodiscard]] auto retry() noexcept
        -> libk::Expected<void, PageStateError>;
    [[nodiscard]] auto mark_dirty(u64 epoch) noexcept
        -> libk::Expected<void, PageStateError>;
    [[nodiscard]] auto queue_writeback() noexcept
        -> libk::Expected<WritebackKey, PageStateError>;
    [[nodiscard]] auto begin_writeback_publish(WritebackKey key) noexcept
        -> libk::Expected<void, PageStateError>;
    [[nodiscard]] auto publish_writeback(
        WritebackKey key,
        u64 delivery_generation) noexcept
        -> libk::Expected<void, PageStateError>;
    [[nodiscard]] auto abort_writeback_publish(WritebackKey key) noexcept
        -> libk::Expected<void, PageStateError>;
    [[nodiscard]] auto claim_writeback(
        WritebackKey key,
        u64 delivery_generation,
        u64 claim_generation) noexcept
        -> libk::Expected<void, PageStateError>;
    [[nodiscard]] auto requeue_writeback(
        WritebackKey key,
        u64 delivery_generation,
        u64 claim_generation) noexcept
        -> libk::Expected<void, PageStateError>;
    [[nodiscard]] auto begin_writeback_complete(
        WritebackKey key,
        u64 delivery_generation,
        u64 claim_generation) noexcept
        -> libk::Expected<void, PageStateError>;
    [[nodiscard]] auto fail_writeback(
        WritebackKey key,
        u64 delivery_generation,
        u64 claim_generation,
        WritebackFailure failure) noexcept
        -> libk::Expected<void, PageStateError>;
    [[nodiscard]] auto complete_writeback(
        WritebackKey key,
        u64 delivery_generation,
        u64 claim_generation) noexcept
        -> libk::Expected<void, PageStateError>;
    [[nodiscard]] auto retain_reclaim() noexcept
        -> libk::Expected<void, PageStateError>;
    [[nodiscard]] auto begin_evict() noexcept
        -> libk::Expected<void, PageStateError>;
    [[nodiscard]] auto finish_evict() noexcept
        -> libk::Expected<void, PageStateError>;
    [[nodiscard]] auto detach() noexcept
        -> libk::Expected<void, PageStateError>;
    [[nodiscard]] auto release() noexcept
        -> libk::Expected<void, PageStateError>;
};

} // namespace kernel::mm
