#pragma once

#include <core/types.hpp>
#include <libk/expected.hpp>
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

struct RequestLink final {
    using Finish = void (*)(void*, mm::PageKey, bool failed) noexcept;

    void* context{};
    Finish finish{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return context != nullptr && finish != nullptr;
    }
};

struct Request final {
    RequestKey key{};
    mm::PageKey page_key{};
    usize first{};
    usize count{};
    u64 backing_epoch{};
    u8 urgency{};
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

enum class State : u8 {
    Open,
    Closing,
    Closed,
};

// Pager is the bounded transport owner.  It never stores page bytes or PTEs;
// MemoryObject owns those truths and uses RequestKey only as a checked edge.
class Pager final : private libk::noncopyable_nonmovable {
    struct Slot final {
        Request request{};
        u64 generation{};
        bool occupied{};
        bool claimed{};
        RequestLink link{};
    };

public:
    Pager() noexcept;
    ~Pager() noexcept;

    [[nodiscard]] auto state() const noexcept -> State;
    [[nodiscard]] auto pending() const noexcept -> usize;
    [[nodiscard]] auto publish(
        mm::PageKey page_key,
        usize first,
        usize count,
        u64 backing_epoch,
        u8 urgency = 0,
        RequestLink link = {}) noexcept -> libk::Expected<Request, Error>;
    [[nodiscard]] auto try_claim() noexcept
        -> libk::Expected<Request, Error>;
    [[nodiscard]] auto claimed(RequestKey key) const noexcept -> bool;
    [[nodiscard]] auto claimed(
        mm::PageKey page_key,
        u64 claim_generation) const noexcept -> bool;
    [[nodiscard]] auto bind(
        ipc::Notification& notification,
        u64 badge) noexcept -> libk::Expected<void, Error>;
    [[nodiscard]] auto unbind() noexcept -> bool;
    [[nodiscard]] auto claim(RequestKey key) noexcept
        -> libk::Expected<Request, Error>;
    [[nodiscard]] auto requeue(RequestKey key) noexcept
        -> libk::Expected<void, Error>;
    [[nodiscard]] auto complete(RequestKey key) noexcept
        -> libk::Expected<void, Error>;
    [[nodiscard]] auto complete(
        mm::PageKey page_key,
        u64 claim_generation) noexcept -> libk::Expected<void, Error>;
    [[nodiscard]] auto fail(RequestKey key) noexcept
        -> libk::Expected<void, Error>;
    [[nodiscard]] auto fail(
        mm::PageKey page_key,
        u64 claim_generation) noexcept -> libk::Expected<void, Error>;
    // Detach a MemoryObject-owned callback before its backing storage is
    // reclaimed. Requests remain transport-visible, but late completion no
    // longer dereferences the retired backing.
    void detach_links(void* context) noexcept;
    [[nodiscard]] auto close(bool force) noexcept -> bool;
    void retire(object::ObjectCleanup&& cleanup) noexcept;

private:
    void notification_closed() noexcept;
    [[nodiscard]] auto find_locked(RequestKey key) noexcept -> Slot*;
    [[nodiscard]] auto find_page_locked(
        mm::PageKey page_key,
        u64 claim_generation) noexcept -> Slot*;
    [[nodiscard]] auto find_free_locked() noexcept -> Slot*;
    [[nodiscard]] auto finish_locked(
        Slot& slot,
        RequestLink& link,
        mm::PageKey& page_key) noexcept -> bool;

    mutable kernel::sync::SpinLock<kernel::sync::LockClass::Pager>
        lock_{};
    Slot slots_[max_requests]{};
    libk::InplaceRing<u16, max_requests> ready_{};
    ipc::NotificationSource source_link_;
    ipc::Notification* notification_{};
    u64 badge_{};
    State state_{State::Open};
    usize claimed_{};
    object::ObjectCleanup cleanup_{};
};

} // namespace kernel::pager
