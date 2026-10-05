#pragma once

#include <cpu/ipi_delivery.hpp>
#include <cpu/types.hpp>
#include <libk/assert.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/noncopyable.hpp>
#include <optional>
#include <libk/sync/atomic.hpp>
#include <sync.hpp>

namespace sched {

enum class RemoteKind : u8 { Start, Wake, Stop };
enum class RemoteCancel : u8 { CanceledQueued, AlreadyClaimed, NotPending };
enum class RemotePost : u8 { Inserted, Coalesced };

// Embedded in its real owner. Pending includes both queued and claimed work;
// only consumer completion permits another post to reuse the intrusive hook.
class RemoteRequest final : private libk::noncopyable_nonmovable {
public:
    RemoteRequest(RemoteKind, void* owner) noexcept;
    ~RemoteRequest() noexcept;
    [[nodiscard]] auto kind() const noexcept -> RemoteKind {
        return static_cast<RemoteKind>(owner_kind_ & kind_mask);
    }
    [[nodiscard]] auto owner() const noexcept -> void* {
        return reinterpret_cast<void*>(owner_kind_ & ~kind_mask);
    }
    [[nodiscard]] auto pending() const noexcept -> bool {
        return pending_.load<libk::MemoryOrder::Acquire>();
    }
private:
    friend class RemoteQueue;
    static constexpr usize kind_mask = 3;
    libk::IntrusiveListHook hook_{};
    usize owner_kind_{};
    libk::Atomic<bool> pending_{};
};

// Queue membership is authoritative. IpiDelivery retains the transport edge,
// including retry when a sender fails outside the queue lock.
class RemoteQueue final : private libk::noncopyable_nonmovable {
    using Queue = libk::IntrusiveList<RemoteRequest, &RemoteRequest::hook_>;
public:
    explicit RemoteQueue(CpuId home) noexcept : home_(home) {}
    [[nodiscard]] auto post(RemoteRequest&) noexcept -> RemotePost;
    [[nodiscard]] auto claim_transport() noexcept -> std::optional<IpiDelivery::Token>;
    void transport_failed(IpiDelivery::Token) noexcept;
    [[nodiscard]] auto take() noexcept -> RemoteRequest*;
    void complete(RemoteRequest&) noexcept;
    [[nodiscard]] auto cancel(RemoteRequest&) noexcept -> RemoteCancel;
    [[nodiscard]] auto size() const noexcept -> usize;
private:
    mutable sync::Spin lock_{};
    Queue queue_{};
    IpiDelivery delivery_{};
    CpuId home_{};
};

} // namespace sched
