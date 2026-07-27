#pragma once

#include <core/types.hpp>
#include <libk/expected.hpp>

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
    WritebackActive,
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

struct PageRequest final {
    PageKey key{};
    usize first{};
    usize count{};
    u64 claim_generation{};
    u32 waiters{};
    u16 transport_slot{};
    bool published{};
    bool claimed{};
    bool failed{};

    [[nodiscard]] constexpr auto contains(usize page) const noexcept -> bool {
        return page >= first && page - first < count;
    }
};

struct PageSlot final {
    PageSlotState state{PageSlotState::Missing};
    u64 generation{};
    u64 content_epoch{};
    u64 dirty_epoch{};
    u64 writeback_epoch{};
    u16 transport_slot{};
    PageRequest request{};

    [[nodiscard]] auto begin_request(
        PageKey key,
        usize first,
        usize count) noexcept
        -> libk::Expected<PageRequest*, PageStateError>;
    void cancel_request() noexcept;
    [[nodiscard]] auto claim_request(u64 generation) noexcept
        -> libk::Expected<void, PageStateError>;
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
        -> libk::Expected<u64, PageStateError>;
    [[nodiscard]] auto begin_writeback(u64 epoch) noexcept
        -> libk::Expected<void, PageStateError>;
    [[nodiscard]] auto complete_writeback(u64 epoch, bool clean) noexcept
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
