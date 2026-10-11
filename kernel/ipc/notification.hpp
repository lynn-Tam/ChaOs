#pragma once

#include <optional>


#include <libk/types.hpp>
#include <cap/grant.hpp>
#include <ipc/notification_source.hpp>
#include <expected>
#include <libk/intrusive_list.hpp>
#include <libk/noncopyable.hpp>
#include <atomic>
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
    [[nodiscard]] auto wait(Thread&, Cpus&, sched::Dispatcher&, std::optional<u64> deadline = std::nullopt) noexcept
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
        libk::intrusive_list_hook hook_{};

        WaitResult result_{};
        Completion relation_;
        sched::Deadline deadline_;
    };
    using Waiters = libk::intrusive_list<Wait, &Wait::hook_>;

    using Sources = libk::intrusive_list<
        NotificationSource, &NotificationSource::hook_>;

    void release_wait() noexcept;
    void detach_source(NotificationSource& source, bool notify) noexcept;
    void retain_relation() noexcept;
    void release_relation() noexcept;
    void try_finish_retire() noexcept;

    u64 pending_{};
    u64 signal_sequence_{};
    std::atomic<Life> life_{Life::Open};
    std::atomic<usize> signalers_{};
    std::atomic<usize> relations_{};
    mutable sync::Spin
        receiver_lock_{};
    Sources sources_{};
    Waiters waiters_{};
    usize waiter_count_{};
    object::cleanup cleanup_{};

};

} // namespace ipc
