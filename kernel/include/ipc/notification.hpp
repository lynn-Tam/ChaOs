#pragma once

#include <core/types.hpp>
#include <cap/grant.hpp>
#include <cap/resolved.hpp>
#include <ipc/notification_link.hpp>
#include <libk/expected.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/noncopyable.hpp>
#include <libk/sync/atomic.hpp>
#include <libk/sync/ticket_spin_lock.hpp>
#include <object/object_cleanup.hpp>
#include <object/vproc_pool.hpp>
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

struct NotificationTake final {
    u64 badges{};
    u64 sequence{};
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
    [[nodiscard]] auto take(Vproc* current = nullptr) noexcept
        -> libk::Expected<NotificationTake, NotificationError>;
    [[nodiscard]] auto wait(
        Thread& thread,
        CpuRegistry& cpus) noexcept
        -> libk::Expected<NotificationWait, NotificationError>;
    [[nodiscard]] auto bind_vproc(
        Vproc& vproc,
        CpuRegistry& cpus,
        const cap::Resolved<Notification>& authority,
        usize slot,
        usize tag) noexcept -> libk::Expected<void, NotificationError>;
    [[nodiscard]] auto unbind_vproc(Vproc& vproc) noexcept
        -> libk::Expected<void, NotificationError>;

    [[nodiscard]] auto bind(
        NotificationSource& source,
        u64 badge) noexcept -> libk::Expected<void, NotificationError>;
    void retire(object::ObjectCleanup&& cleanup) noexcept;

private:
    friend class NotificationSource;
    friend class kernel::Vproc;

    enum class Life : u8 {
        Open,
        Closing,
        Closed,
    };

    enum class Receiver : u8 {
        None,
        Attaching,
        Async,
        Detaching,
        Draining,
    };

    struct AuthorityLink final : private libk::noncopyable_nonmovable {
        AuthorityLink(
            Notification& owner,
            const cap::GrantAttachmentOps& ops) noexcept
            : owner(&owner), attachment(this, ops) {}

        Notification* owner{};
        cap::GrantAttachment attachment;
        cap::GrantWork work{};
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
    static void invalidate(
        void* context,
        cap::GrantWork&& work,
        cap::GrantInvalidation reason) noexcept;
    static void released(void* context) noexcept;
    void invalidated(cap::GrantWork&& work) noexcept;
    void begin_async_detach() noexcept;
    void try_finish_async_detach() noexcept;
    void async_publisher_done() noexcept;
    void peer_stopped(Vproc& vproc) noexcept;
    void retain_relation() noexcept;
    void release_relation() noexcept;
    void try_finish_retire() noexcept;

    libk::Atomic<u64> pending_{};
    u64 signal_sequence_{};
    libk::Atomic<Life> life_{Life::Open};
    libk::Atomic<usize> signalers_{};
    libk::Atomic<usize> relations_{};
    mutable libk::TicketSpinLock receiver_lock_{};
    Sources sources_{};
    Wait wait_;
    Receiver receiver_{Receiver::None};
    Vproc* target_{};
    object::ObjectHold<Vproc> target_hold_{};
    NotificationLink target_link_;
    AuthorityLink authority_;
    CpuRegistry* cpus_{};
    usize slot_{};
    usize tag_{};
    u64 binding_generation_{};
    usize async_publishers_{};
    object::ObjectCleanup cleanup_{};

    static const cap::GrantAttachmentOps authority_ops_;
};

} // namespace kernel::ipc
