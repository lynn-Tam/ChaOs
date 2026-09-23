#pragma once

#include <cap/cspace.hpp>
#include <cap/resolved.hpp>
#include <core/types.hpp>
#include <ipc/notification.hpp>
#include <ipc/channel_wait.hpp>
#include <libk/array.hpp>
#include <libk/expected.hpp>
#include <libk/inplace_vector.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/noncopyable.hpp>
#include <libk/optional.hpp>
#include <mm/node_pool.hpp>
#include <mm/pmm.hpp>
#include <object/object_cleanup.hpp>
#include <object/object_ref.hpp>
#include <operation/completion.hpp>
#include <sync/lock.hpp>
#include <uapi/channel.h>

namespace kernel {
class CpuRegistry;
class Thread;
}

namespace kernel::ipc {

class Buffer;

using ChannelSide = cap::ChannelSide;

enum class ChannelError : u8 {
    Closed,
    PeerClosed,
    WouldBlock,
    Invalid,
    Denied,
    Busy,
    ResourceExhausted,
    TransferFailed,
    InvalidRelation,
    GenerationExhausted,
};

enum class ChannelCondition : u8 {
    Readable = MYOS_CHANNEL_READABLE,
    Writable = MYOS_CHANNEL_WRITABLE,
    PeerClosed = MYOS_CHANNEL_PEER_CLOSED,
};

struct ChannelConfig final {
    usize queue_capacity{16};
    usize max_words{MYOS_CHANNEL_MAX_WORDS};
    usize max_caps{MYOS_CHANNEL_MAX_CAPS};
    usize relation_capacity{4};
};

struct ChannelWaitResult final {
    kernel::operation::State state{kernel::operation::State::Complete};
    u64 value{};
};

// A bounded bidirectional queue. Channel owns all queue cells, capability
// escrow, readiness relations, and protocol-close state; Notification only
// receives level/sequence hints through NotificationSource.
class Channel final : private libk::noncopyable_nonmovable {
    enum class CommitResult : u8 {
        Committed,
        Capacity,
        Invalid,
    };

    struct Escrow final : private libk::noncopyable {
        enum class Kind : u8 {
            Copy,
            Delegate,
            Move,
        };

        Escrow() noexcept = default;
        Escrow(Escrow&&) noexcept = default;
        auto operator=(Escrow&&) noexcept -> Escrow& = default;

        cap::CSpace* source{};
        cap::CSpace::Reservation source_slot{};
        cap::GrantRef grant{};
        cap::CapView view{};
        cap::CapHandle source_handle{};
        Kind kind{Kind::Copy};
    };

    struct Message final : private libk::noncopyable {
        libk::IntrusiveListHook hook{};
        ChannelSide destination{ChannelSide::A};
        u64 transaction{};
        u64 tag{};
        u64 sender_badge{};
        u64 sequence{};
        usize word_count{};
        u64 words[MYOS_CHANNEL_MAX_WORDS]{};
        libk::InplaceVector<Escrow, MYOS_CHANNEL_MAX_CAPS> escrows{};
    };

    struct Relation;

    struct AuthLink final : private libk::noncopyable_nonmovable {
        AuthLink(Relation& relation, bool channel) noexcept;
        ~AuthLink() noexcept = default;

        Relation* relation{};
        bool channel{};
        cap::GrantAttachment attachment;
        cap::GrantWork work{};
    };

    struct SideLink final : private libk::noncopyable_nonmovable {
        SideLink(Channel& owner, ChannelSide value) noexcept;
        ~SideLink() noexcept = default;

        Channel* owner{};
        ChannelSide side{ChannelSide::A};
        cap::GrantAttachment attachment;
        cap::GrantWork work{};
    };

    struct Relation final : private libk::noncopyable_nonmovable {
        Relation() noexcept;
        ~Relation() noexcept;

        void closed() noexcept;
        void notification_closed() noexcept;

        Channel* owner{};
        libk::IntrusiveListHook hook{};
        usize index{};
        NotificationSource source;
        object::ObjectHold<Notification> notification{};
        AuthLink channel_link;
        AuthLink notification_link;
        ChannelSide side{ChannelSide::A};
        ChannelCondition condition{ChannelCondition::Readable};
        u64 generation{};
        u64 observed{};
        enum class State : u8 {
            Idle,
            Attaching,
            Attached,
            Detaching,
        };
        State state{State::Idle};
        bool armed{};
    };

    using MessageQueue = libk::IntrusiveList<Message, &Message::hook>;
    struct Side final {
        MessageQueue queue{};
        usize occupied{}; // Queued, preparing, or finishing a receive.
        u64 sequence[3]{};
        bool closed{};
    };

    friend struct ChannelWait;
    using Waiter = ChannelWait;

public:
    [[nodiscard]] static auto storage_bytes(ChannelConfig config) noexcept -> libk::optional<usize>;
    Channel(kernel::mm::Pmm& pmm, ChannelConfig config = {}) noexcept;
    ~Channel() noexcept;

    [[nodiscard]] auto open() noexcept -> libk::Expected<void, ChannelError>;
    [[nodiscard]] auto send(
        cap::Resolved<Channel>& authority,
        cap::CSpace& source,
        const ChannelSend& request) noexcept
        -> libk::Expected<u64, ChannelError>;
    [[nodiscard]] auto send_blocking(
        cap::Resolved<Channel>& authority,
        cap::CapHandle authority_handle,
        cap::CSpace& source,
        kernel::Thread& thread,
        kernel::CpuRegistry& cpus,
        const ChannelSend& request) noexcept
        -> libk::Expected<ChannelWaitResult, ChannelError>;
    [[nodiscard]] auto receive(
        cap::Resolved<Channel>& authority,
        cap::CSpace& destination,
        ChannelRecv& result) noexcept
        -> libk::Expected<void, ChannelError>;
    [[nodiscard]] auto receive_blocking(
        cap::Resolved<Channel>& authority,
        cap::CapHandle authority_handle,
        cap::CSpace& destination,
        kernel::Thread& thread,
        kernel::CpuRegistry& cpus,
        Buffer* buffer,
        ChannelRecv& result) noexcept
        -> libk::Expected<ChannelWaitResult, ChannelError>;
    [[nodiscard]] auto close(
        cap::Resolved<Channel>& authority) noexcept
        -> libk::Expected<void, ChannelError>;
    [[nodiscard]] auto close(ChannelSide side) noexcept -> bool;

    [[nodiscard]] auto bind(
        cap::Resolved<Channel>& authority,
        cap::Resolved<Notification>& notification,
        ChannelCondition condition) noexcept
        -> libk::Expected<usize, ChannelError>;
    [[nodiscard]] auto arm(
        cap::Resolved<Channel>& authority,
        usize relation,
        u64 observed) noexcept -> libk::Expected<u64, ChannelError>;

    [[nodiscard]] auto mint(
        cap::Resolved<Channel>& authority,
        cap::CSpace& destination,
        u64 badge,
        cap::Rights rights) noexcept
        -> libk::Expected<cap::CapHandle, ChannelError>;

    // Bound during construction. Revoking the exact side-root grant closes
    // that protocol side through the authority graph's lifecycle edge.
    [[nodiscard]] auto bind_side_root(
        cap::GrantRef& root,
        ChannelSide side) noexcept -> bool;

    void bind_sponsor(kernel::resource::Sponsorship& sponsor) noexcept;
    void retire(object::ObjectCleanup&& cleanup) noexcept;

    [[nodiscard]] auto side_state(ChannelSide side) const noexcept
        -> bool;
    [[nodiscard]] auto config() const noexcept -> ChannelConfig {
        return config_;
    }

private:
    static void invalidate(
        void* context,
        cap::GrantWork&& work,
        cap::GrantInvalidation reason) noexcept;
    static void released(void* context) noexcept;
    void invalidated(AuthLink& link, cap::GrantWork&& work) noexcept;
    void relation_released(Relation& relation) noexcept;
    static void invalidate_side(
        void* context,
        cap::GrantWork&& work,
        cap::GrantInvalidation reason) noexcept;
    static void release_side(void* context) noexcept;
    void side_invalidated(SideLink& link, cap::GrantWork&& work) noexcept;
    void side_released(SideLink& link) noexcept;
    static void invalidate_waiter(
        void* context,
        cap::GrantWork&& work,
        cap::GrantInvalidation reason) noexcept;
    static void release_waiter(void* context) noexcept;
    void waiter_invalidated(Waiter& waiter, cap::GrantWork&& work) noexcept;
    void drop_waiter(Waiter& waiter) noexcept;
    void detach_waiter_authority(Waiter& waiter) noexcept;
    void finish_waiter(Waiter& waiter) noexcept;
    void abort_relation(Relation& relation) noexcept;
    void detach_side(SideLink& link) noexcept;
    void try_finish_retire() noexcept;
    using WaitQueue = libk::IntrusiveList<Waiter, &Waiter::hook>;
    [[nodiscard]] auto wait_queue(Waiter::Kind kind, ChannelSide side) noexcept -> WaitQueue&;
    [[nodiscard]] auto enqueue_waiter(
        cap::Resolved<Channel>& authority, cap::CapHandle handle,
        cap::CSpace& cspace, kernel::Thread& thread, Waiter::Kind kind) noexcept
        -> libk::Expected<Waiter*, ChannelError>;
    [[nodiscard]] auto begin_wait(
        Waiter& waiter, cap::Resolved<Channel>& authority,
        kernel::Thread& thread, kernel::CpuRegistry& cpus) noexcept
        -> libk::Expected<ChannelWaitResult, ChannelError>;
    [[nodiscard]] auto owns_turn_locked(
        Waiter::Kind kind, ChannelSide side, const Waiter* reservation) noexcept -> bool;
    [[nodiscard]] auto waiter_ready_locked(
        const Waiter& waiter) noexcept -> bool;
    [[nodiscard]] auto send_impl(
        cap::Resolved<Channel>& authority,
        cap::CSpace& source,
        const ChannelSend& request,
        Waiter* reservation) noexcept
        -> libk::Expected<u64, ChannelError>;
    [[nodiscard]] auto receive_impl(
        cap::Resolved<Channel>& authority,
        cap::CSpace& destination,
        ChannelRecv& result,
        Waiter* reservation) noexcept
        -> libk::Expected<void, ChannelError>;
    [[nodiscard]] auto arm_waiter(Waiter& waiter) noexcept -> bool;
    [[nodiscard]] auto resume_waiter(
        Waiter& waiter,
        arch::TrapContext& trap) noexcept -> bool;

    [[nodiscard]] auto side(ChannelSide value) noexcept -> Side&;
    [[nodiscard]] auto side(ChannelSide value) const noexcept -> const Side&;
    [[nodiscard]] auto side_index(ChannelSide value) const noexcept -> usize;
    [[nodiscard]] auto peer(ChannelSide value) const noexcept -> ChannelSide;
    [[nodiscard]] auto authority_side(
        const cap::Resolved<Channel>& authority,
        cap::Right right) const noexcept
        -> libk::optional<ChannelSide>;
    [[nodiscard]] auto ready_locked(
        ChannelSide side,
        ChannelCondition condition) const noexcept -> bool;
    [[nodiscard]] auto sequence_locked(
        ChannelSide side,
        ChannelCondition condition) const noexcept -> u64;
    void notify_ready();
    void detach_relation(Relation& relation) noexcept;
    void finish_relation(Relation& relation) noexcept;
    void discard_message(Message& message) noexcept;
    [[nodiscard]] auto relation_at(usize index) noexcept -> Relation&;
    void release_message(Message& message) noexcept;
    void clear_queues() noexcept;
    [[nodiscard]] auto make_escrow(
        cap::CSpace& source,
        const myos_cap_transfer& spec,
        Escrow& escrow) noexcept -> libk::Expected<void, ChannelError>;
    [[nodiscard]] auto commit_escrows(
        Message& message,
        cap::CSpace& destination,
        libk::InplaceVector<cap::CSpace::Reservation,
            MYOS_CHANNEL_MAX_CAPS>& reservations,
        ChannelRecv& result) noexcept -> CommitResult;

    static const cap::GrantAttachmentOps channel_ops_;
    static const cap::GrantAttachmentOps notification_ops_;
    static const cap::GrantAttachmentOps side_ops_;
    static const cap::GrantAttachmentOps waiter_ops_;

    ChannelConfig config_{};
    mm::NodePool<Message> messages_;
    mutable kernel::sync::SpinLock<kernel::sync::LockClass::Channel> lock_{};
    Side sides_[2]{};
    SideLink side_links_[2];
    mm::NodePool<Relation> relation_pool_;
    libk::IntrusiveList<Relation, &Relation::hook> relations_{};
    WaitQueue wait_queues_[2][2]{};
    MessageQueue free_messages_{};
    usize waiter_count_{};
    bool opened_{};
    bool closing_{};
    object::ObjectCleanup cleanup_{};
};

} // namespace kernel::ipc
