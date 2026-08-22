#pragma once

#include <core/types.hpp>
#include <libk/expected.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/inplace_ring.hpp>
#include <libk/noncopyable.hpp>
#include <sync/lock.hpp>
#include <object/object_cleanup.hpp>
#include <mm/page_state.hpp>
#include <ipc/notification.hpp>

namespace kernel::pager {

inline constexpr usize max_requests = 32;
inline constexpr usize max_request_pages = 16;

struct RequestKey final {
    u16 slot{};
    u64 generation{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return generation != 0;
    }
    [[nodiscard]] friend constexpr auto operator==(
        RequestKey, RequestKey) noexcept -> bool = default;
};

struct ClaimKey final {
    RequestKey delivery{};
    u64 generation{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return delivery && generation != 0;
    }
    [[nodiscard]] friend constexpr auto operator==(
        ClaimKey, ClaimKey) noexcept -> bool = default;
};

enum class DeliveryKind : u8 {
    PageIn,
    Writeback,
};

struct Request;

struct PagerAttachment final {
    enum class Event : u8 {
        Claim,
        Requeue,
        Forced,
    };
    // Runs after Pager pins the exact delivery relation and before it exposes
    // the transport edge.  It reports only the MemoryObject owner edge.
    using Transition = bool (*)(void*, const Request&, Event) noexcept;
    using Drained = void (*)(void*) noexcept;
    /*luna change: expose one narrow producer-ready edge, reason: kernel
      transport capacity must wake the existing owner executor without using
      the userspace notification*/
    using Ready = void (*)(void*) noexcept;

    libk::IntrusiveListHook hook_{};
    /*luna change: index one producer waiting for transport capacity, reason:
      Pager is the sole slot-capacity owner and can wake MemoryExecutor O(1)*/
    libk::IntrusiveListHook capacity_hook_{};

    void* context{};
    Transition transition{};
    Drained drained{};
    /*luna change: keep the producer callback on the existing attachment,
      reason: capacity waiters borrow the established attachment lifetime*/
    Ready ready{};
    u64 generation{};
    u32 leases{};
    enum class State : u8 {
        Detached,
        Attached,
        Retiring,
    } state{State::Detached};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        /*luna change: require an exact producer wake edge for attachment
          admission, reason: a Full publication must never strand owner work*/
        return context != nullptr && transition != nullptr
            && drained != nullptr && ready != nullptr;
    }
};

struct Request final {
    RequestKey key{};
    ClaimKey claim{};
    DeliveryKind kind{DeliveryKind::PageIn};
    mm::PageKey page_key{};
    usize first{};
    usize count{};
    u64 backing_epoch{};
    u64 writeback_generation{};
    u64 dirty_epoch{};
    u8 urgency{};

    [[nodiscard]] friend constexpr auto operator==(
        const Request&, const Request&) noexcept -> bool = default;
};

enum class Error : u8 {
    Closed,
    Full,
    InvalidKey,
    Busy,
    Stale,
    InvalidRange,
    GenerationExhausted,
};

class Pager;

class Reply final : private libk::noncopyable {
public:
    Reply() noexcept = default;
    Reply(Reply&& other) noexcept;
    auto operator=(Reply&& other) noexcept -> Reply&;
    ~Reply() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept {
        return pager_ != nullptr && key_;
    }
    [[nodiscard]] auto key() const noexcept -> ClaimKey { return key_; }
    [[nodiscard]] auto request() const noexcept -> const Request& { return request_; }
    [[nodiscard]] auto attachment() const noexcept -> PagerAttachment* {
        return attachment_;
    }
    [[nodiscard]] auto commit() noexcept -> libk::Expected<void, Error>;
    [[nodiscard]] auto abort() noexcept -> libk::Expected<void, Error>;

private:
    friend class Pager;
    Reply(
        Pager& pager,
        Request request,
        PagerAttachment* attachment) noexcept
        : pager_(&pager),
          key_(request.claim),
          request_(request),
          attachment_(attachment) {}

    Pager* pager_{};
    ClaimKey key_{};
    Request request_{};
    PagerAttachment* attachment_{};
};

enum class State : u8 {
    Open,
    Closing,
    Forced,
    Closed,
};

// Pager is the bounded transport owner.  It never stores page bytes or PTEs;
// MemoryObject owns those truths and uses RequestKey only as a checked edge.
class Pager final : private libk::noncopyable_nonmovable {
    friend class Reply;
    enum class TransportState : u8 {
        Free,
        Queued,
        Claimed,
        Completing,
    };
    struct Slot final {
        u64 generation{};
        u64 claim_generation{};
        TransportState state{TransportState::Free};
        u32 leases{};
        PagerAttachment* attachment{};
        u64 attachment_generation{};
        union Payload final {
            struct PageIn final {
                usize first{};
                usize count{};
                u64 backing_epoch{};
            } page_in;
            struct Writeback final {
                u64 generation{};
                u64 dirty_epoch{};
            } writeback;

            constexpr Payload() noexcept : page_in{} {}
        } payload{};
        usize page_index{};
        u64 page_generation{};
        DeliveryKind kind{DeliveryKind::PageIn};
        u8 urgency{};
    };

public:
    Pager() noexcept;
    ~Pager() noexcept;

    [[nodiscard]] auto state() const noexcept -> State;
    [[nodiscard]] auto pending() const noexcept -> usize;
    [[nodiscard]] auto publish(
        PagerAttachment& attachment,
        mm::PageKey page_key,
        usize first,
        usize count,
        u64 backing_epoch,
        u8 urgency = 0) noexcept -> libk::Expected<Request, Error> {
        return publish(
            &attachment,
            DeliveryKind::PageIn,
            page_key,
            first,
            count,
            backing_epoch,
            0,
            0,
            urgency);
    }
    [[nodiscard]] auto publish(
        mm::PageKey page_key,
        usize first,
        usize count,
        u64 backing_epoch,
        u8 urgency = 0) noexcept -> libk::Expected<Request, Error> {
        return publish(
            nullptr,
            DeliveryKind::PageIn,
            page_key,
            first,
            count,
            backing_epoch,
            0,
            0,
            urgency);
    }
    [[nodiscard]] auto publish_writeback(
        PagerAttachment& attachment,
        mm::PageKey page_key,
        u64 writeback_generation,
        u64 dirty_epoch,
        u8 urgency = 0) noexcept -> libk::Expected<Request, Error> {
        return publish(
            &attachment,
            DeliveryKind::Writeback,
            page_key,
            page_key.index,
            1,
            dirty_epoch,
            writeback_generation,
            dirty_epoch,
            urgency);
    }
    [[nodiscard]] auto publish_writeback(
        mm::PageKey page_key,
        u64 writeback_generation,
        u64 dirty_epoch,
        u8 urgency = 0) noexcept -> libk::Expected<Request, Error> {
        return publish(
            nullptr,
            DeliveryKind::Writeback,
            page_key,
            page_key.index,
            1,
            dirty_epoch,
            writeback_generation,
            dirty_epoch,
            urgency);
    }
    [[nodiscard]] auto attach(PagerAttachment& attachment) noexcept -> bool;
    [[nodiscard]] auto detach(PagerAttachment& attachment) noexcept -> bool;
    [[nodiscard]] auto try_claim() noexcept
        -> libk::Expected<Request, Error>;
    [[nodiscard]] auto begin_reply(ClaimKey key) noexcept
        -> libk::Expected<Reply, Error>;
    [[nodiscard]] auto bind(
        ipc::Notification& notification,
        u64 badge) noexcept -> libk::Expected<void, Error>;
    [[nodiscard]] auto unbind() noexcept -> bool;
    [[nodiscard]] auto cancel(RequestKey key) noexcept
        -> libk::Expected<void, Error>;
    [[nodiscard]] auto requeue(
        ClaimKey key,
        const Request& request,
        PagerAttachment* expected_attachment = nullptr) noexcept
        -> libk::Expected<void, Error>;
    [[nodiscard]] auto close(bool force) noexcept -> bool;
    void retire(object::ObjectCleanup&& cleanup) noexcept;

private:
    void notification_closed() noexcept;
    [[nodiscard]] auto publish(
        PagerAttachment* attachment,
        DeliveryKind kind,
        mm::PageKey page_key,
        usize first,
        usize count,
        u64 backing_epoch,
        u64 writeback_generation,
        u64 dirty_epoch,
        u8 urgency) noexcept -> libk::Expected<Request, Error>;
    [[nodiscard]] auto find_locked(RequestKey key) noexcept -> Slot*;
    [[nodiscard]] auto find_claim_locked(ClaimKey key) noexcept -> Slot*;
    [[nodiscard]] auto find_free_locked() noexcept -> Slot*;
    [[nodiscard]] auto finish_locked(
        Slot& slot,
        PagerAttachment*& attachment,
        Request& request,
        bool& drained) noexcept -> bool;
    [[nodiscard]] auto finish_reply(Reply& reply) noexcept
        -> libk::Expected<void, Error>;
    [[nodiscard]] auto abort_reply(Reply& reply) noexcept
        -> libk::Expected<void, Error>;
    [[nodiscard]] auto view(const Slot& slot, u16 index) const noexcept
        -> Request;
    /*luna change: wake one capacity waiter through an attachment lease,
      reason: producer retry is bounded and foreign callbacks stay lock-free*/
    void wake_capacity() noexcept;

    mutable kernel::sync::SpinLock<kernel::sync::LockClass::Pager>
        lock_{};
    Slot slots_[max_requests]{};
    libk::InplaceRing<u16, max_requests> ready_{};
    ipc::NotificationSource source_link_;
    ipc::Notification* notification_{};
    u64 badge_{};
    State state_{State::Open};
    usize claimed_{};
    using Attachments = libk::IntrusiveList<
        PagerAttachment, &PagerAttachment::hook_>;
    Attachments attachments_{};
    using CapacityWaiters = libk::IntrusiveList<
        PagerAttachment, &PagerAttachment::capacity_hook_>;
    /*luna change: derive capacity waiters from attachment ownership, reason:
      slot capacity needs O(1) wake without a second request record*/
    CapacityWaiters capacity_waiters_{};
    object::ObjectCleanup cleanup_{};
};

} // namespace kernel::pager
