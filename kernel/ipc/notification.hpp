#pragma once

#include <optional>


#include <base/types.hpp>
#include <cap/grant.hpp>
#include <ipc/notification_source.hpp>
#include <expected>
#include <libk/intrusive_list.hpp>
#include <libk/noncopyable.hpp>
#include <libk/sync/atomic.hpp>
#include <sync.hpp>
#include <object/ref.hpp>
#include <wait.hpp>
#include <sched/sched.hpp>

class Cpus;
class Thread;

namespace ipc {

class Notification;

enum class NotificationError : u8 {
    Closed,
    Empty,
    Busy,
    InvalidBadge,
};

struct NotificationTake final {
    u64 badges{};
    u64 sequence{};
};

class Notification final : private libk::noncopyable_nonmovable {
public:
    Notification() noexcept = default;
    ~Notification() noexcept;

    // Badge comes from a capability or receiver-owned source relation. It is
    // never accepted from a signal syscall payload.
    [[nodiscard]] auto signal(u64 badge) noexcept -> bool;
    [[nodiscard]] auto take() noexcept
        -> std::expected<NotificationTake, NotificationError>;
    // The caller retains object storage until this blocking call returns.
    [[nodiscard]] auto wait(Thread&, Cpus&, sched::Dispatcher&, std::optional<time::Instant> deadline = std::nullopt) noexcept
        -> WaitResult;
    [[nodiscard]] auto bind(
        NotificationSource& source,
        u64 badge) noexcept -> std::expected<void, NotificationError>;
    void retire(object::cleanup&& cleanup) noexcept;

private:
    friend class NotificationSource;

    enum class Life : u8 {
        Open,
        Closing,
        Closed,
    };

    class Wait final : private libk::noncopyable_nonmovable {
    public:
        explicit Wait(Notification&) noexcept;
        ~Wait() noexcept;
    private:
        friend class Notification;


        void release() noexcept;
        void expire() noexcept;
        [[nodiscard]] auto cancel() noexcept -> bool;
        // receiver_lock_ selects one winner; Completion drains its publication.
        [[nodiscard]] auto ready(WaitResult) noexcept -> bool;

        Notification* owner_;
        libk::IntrusiveListHook hook_{};

        WaitResult result_{};
        Completion relation_;
        sched::Deadline deadline_;
    };
    using Waiters = libk::IntrusiveList<Wait, &Wait::hook_>;

    using Sources = libk::IntrusiveList<
        NotificationSource, &NotificationSource::hook_>;

    void release_wait() noexcept;
    void detach_source(NotificationSource& source, bool notify) noexcept;
    void retain_relation() noexcept;
    void release_relation() noexcept;
    void try_finish_retire() noexcept;

    u64 pending_{};
    u64 signal_sequence_{};
    libk::Atomic<Life> life_{Life::Open};
    libk::Atomic<usize> signalers_{};
    libk::Atomic<usize> relations_{};
    mutable sync::Spin
        receiver_lock_{};
    Sources sources_{};
    Waiters waiters_{};
    usize waiter_count_{};
    object::cleanup cleanup_{};

};

} // namespace ipc
