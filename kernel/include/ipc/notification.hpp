#pragma once

#include <core/types.hpp>
#include <libk/expected.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/noncopyable.hpp>
#include <libk/sync/atomic.hpp>
#include <libk/sync/ticket_spin_lock.hpp>
#include <object/object_cleanup.hpp>
#include <operation/completion.hpp>

namespace kernel {
class CpuRegistry;
class Thread;
class Vproc;
}

namespace kernel::ipc {

class Notification;

enum class NotificationError : u8 {
    Closed,
    Empty,
    Busy,
    InvalidBadge,
};

struct NotificationWait final {
    operation::State state{};
    u64 badges{};
    operation::Key key{};
};

// A source owns this relation and its canonical readiness state. Notification
// stores only the non-owning aggregation edge. Source teardown must unbind;
// Notification teardown invokes the statically bound closed callback.
class NotificationSource final : private libk::noncopyable_nonmovable {
public:
    template<typename Owner, void (Owner::*Closed)() noexcept>
    [[nodiscard]] static auto bind(Owner& owner) noexcept
        -> NotificationSource {
        static constexpr Ops operations{
            .closed = [](void* context) noexcept {
                (static_cast<Owner*>(context)->*Closed)();
            },
        };
        return NotificationSource{owner, operations};
    }

    ~NotificationSource() noexcept;

    [[nodiscard]] auto attached() const noexcept -> bool;
    [[nodiscard]] auto signal() noexcept -> bool;
    void reset() noexcept;

private:
    friend class Notification;

    struct Ops final {
        void (*closed)(void*) noexcept;
    };

    template<typename Owner>
    explicit NotificationSource(Owner& owner, const Ops& ops) noexcept
        : owner_(&owner), ops_(&ops) {}

    mutable libk::TicketSpinLock lock_{};
    libk::IntrusiveListHook hook_{};
    void* owner_{};
    const Ops* ops_{};
    Notification* notification_{};
    u64 badge_{};
};

class Notification final : private libk::noncopyable_nonmovable {
public:
    Notification() noexcept;
    ~Notification() noexcept;

    // Badge comes from a capability or receiver-owned source relation. It is
    // never accepted from a signal syscall payload.
    [[nodiscard]] auto signal(u64 badge) noexcept -> bool;
    [[nodiscard]] auto take() noexcept
        -> libk::Expected<u64, NotificationError>;
    [[nodiscard]] auto wait(
        Thread& thread,
        CpuRegistry& cpus) noexcept
        -> libk::Expected<NotificationWait, NotificationError>;
    [[nodiscard]] auto wait(
        Vproc& vproc,
        CpuRegistry& cpus,
        usize cookie) noexcept
        -> libk::Expected<NotificationWait, NotificationError>;

    [[nodiscard]] auto bind(
        NotificationSource& source,
        u64 badge) noexcept -> libk::Expected<void, NotificationError>;
    void retire(object::ObjectCleanup&& cleanup) noexcept;

private:
    friend class NotificationSource;

    enum class Life : u8 {
        Open,
        Closing,
        Closed,
    };

    class Wait final : private libk::noncopyable_nonmovable {
    public:
        explicit Wait(Notification& owner) noexcept;

        [[nodiscard]] auto idle() const noexcept -> bool;
        [[nodiscard]] auto complete() const noexcept -> bool;
        void begin() noexcept;
        [[nodiscard]] auto arm() noexcept -> bool;
        [[nodiscard]] auto ready() noexcept -> bool;
        void abort() noexcept;
        [[nodiscard]] auto relation() noexcept -> operation::Completion& {
            return relation_;
        }

    private:
        friend class Notification;

        enum class State : u8 {
            Idle,
            Awaiting,
            Armed,
            Ready,
        };

        [[nodiscard]] auto read() noexcept -> operation::Result;
        void release() noexcept;
        [[nodiscard]] auto cancel() noexcept -> bool;

        Notification* owner_{};
        libk::Atomic<State> state_{State::Idle};
        operation::Completion relation_;
    };

    using Sources = libk::IntrusiveList<
        NotificationSource, &NotificationSource::hook_>;

    [[nodiscard]] auto finish_wait() noexcept -> operation::Result;
    [[nodiscard]] auto cancel_wait() noexcept -> bool;
    void detach_source(NotificationSource& source, bool notify) noexcept;
    void retain_relation() noexcept;
    void release_relation() noexcept;
    void try_finish_retire() noexcept;

    libk::Atomic<u64> pending_{};
    libk::Atomic<Life> life_{Life::Open};
    libk::Atomic<usize> signalers_{};
    libk::Atomic<usize> relations_{};
    mutable libk::TicketSpinLock receiver_lock_{};
    Sources sources_{};
    Wait wait_;
    object::ObjectCleanup cleanup_{};
};

} // namespace kernel::ipc
