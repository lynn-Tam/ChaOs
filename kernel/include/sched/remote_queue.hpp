#pragma once

#include <cpu/ipi_delivery.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/noncopyable.hpp>
#include <libk/optional.hpp>
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

// Embedded in the state owner whose home CPU must commit a remote request.
// The pending bit spans queued and consumed work so producers can coalesce an
// edge without reusing an intrusive hook that the dispatcher still owns.
class RemoteRequest final : private libk::noncopyable_nonmovable {
public:
    RemoteRequest(RemoteKind kind, void* owner) noexcept
        : owner_(owner), kind_(kind) {}
    ~RemoteRequest() noexcept;

    [[nodiscard]] auto kind() const noexcept -> RemoteKind { return kind_; }
    [[nodiscard]] auto owner() const noexcept -> void* { return owner_; }

private:
    friend class RemoteQueue;

    libk::IntrusiveListHook hook_{};
    void* owner_{};
    RemoteKind kind_{};
    bool pending_{};
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
    void post(RemoteRequest& request) noexcept;
    [[nodiscard]] auto claim_transport() noexcept
        -> libk::optional<kernel::IpiDelivery::Token>;
    void transport_failed(kernel::IpiDelivery::Token token) noexcept;
    [[nodiscard]] auto take() noexcept -> RemoteRequest*;
    void complete(RemoteRequest& request) noexcept;
    [[nodiscard]] auto cancel(RemoteRequest& request) noexcept
        -> RemoteCancel;
    [[nodiscard]] auto size() const noexcept -> usize;

private:
    mutable kernel::sync::SpinLock<kernel::sync::LockClass::RemoteQueue>
        lock_{};
    Queue queue_{};
    kernel::IpiDelivery delivery_{};
};

} // namespace kernel::sched
