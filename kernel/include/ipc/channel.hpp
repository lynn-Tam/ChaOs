#pragma once

#include <cap/cspace.hpp>
#include <cap/resolved.hpp>
#include <core/types.hpp>
#include <ipc/notification.hpp>
#include <libk/array.hpp>
#include <libk/expected.hpp>
#include <libk/inplace_ring.hpp>
#include <libk/inplace_vector.hpp>
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
    usize queue_capacity{MYOS_CHANNEL_MAX_QUEUE};
    usize max_words{MYOS_CHANNEL_MAX_WORDS};
    usize max_caps{MYOS_CHANNEL_MAX_CAPS};
    usize waiter_capacity{MYOS_CHANNEL_MAX_WAITERS};
    usize relation_capacity{MYOS_CHANNEL_MAX_RELATIONS};
};

struct ChannelSend final {
    u64 transaction{};
    u64 tag{};
    usize word_count{};
    u64 words[MYOS_CHANNEL_MAX_WORDS]{};
    usize cap_count{};
    myos_cap_transfer caps[MYOS_CHANNEL_MAX_CAPS]{};
};

struct ChannelRecv final {
    u64 transaction{};
    u64 tag{};
    u64 sender_badge{};
    u64 sequence{};
    usize word_count{};
    u64 words[MYOS_CHANNEL_MAX_WORDS]{};
    usize cap_count{};
    usize receive_limit{MYOS_CHANNEL_MAX_CAPS};
    cap::CapHandle caps[MYOS_CHANNEL_MAX_CAPS]{};
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

    struct Side final {
        libk::InplaceRing<Message*, MYOS_CHANNEL_MAX_QUEUE> queue{};
        u64 sequence[3]{};
        bool closed{};
    };

    struct Waiter final : private libk::noncopyable_nonmovable {
        enum class Kind : u8 {
            Idle,
            Send,
            Receive,
        };
        enum class State : u8 {
            Idle,
            Attaching,
            Awaiting,
            Armed,
            Ready,
            Done,
        };

        explicit Waiter(Channel& owner) noexcept;
        ~Waiter() noexcept;

        [[nodiscard]] auto complete() const noexcept -> bool;
        [[nodiscard]] auto read() noexcept -> kernel::operation::Result;
        void release() noexcept;
        [[nodiscard]] auto cancel() noexcept -> bool;
        void resume(arch::TrapContext& trap) noexcept;

        Channel* owner{};
        Kind kind{Kind::Idle};
        State state{State::Idle};
        ChannelSide side{ChannelSide::A};
        cap::CSpace* cspace{};
        cap::CapHandle authority{};
        kernel::Thread* thread{};
        kernel::CpuRegistry* cpus{};
        Buffer* buffer{};
        ChannelSend send{};
        ChannelRecv recv{};
        kernel::operation::Result result{};
        object::ObjectRef channel_ref{};
        cap::GrantAttachment grant_attachment;
        cap::GrantWork grant_work{};
        kernel::operation::Completion completion;
    };

public:
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
        u64 observed) noexcept -> libk::Expected<void, ChannelError>;

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
    void waiter_released(Waiter& waiter) noexcept;
    void finish_waiter(Waiter& waiter) noexcept;
    void abort_relation(Relation& relation) noexcept;
    void detach_side(SideLink& link) noexcept;
    void try_finish_retire() noexcept;
    [[nodiscard]] auto waiter_ready_locked(
        const Waiter& waiter) const noexcept -> bool;
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
    [[nodiscard]] auto take_message() noexcept -> Message*;
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
    Relation relations_[MYOS_CHANNEL_MAX_RELATIONS];
    Waiter waiter_;
    static constexpr usize max_messages = MYOS_CHANNEL_MAX_QUEUE * 2;
    Message* free_messages_[max_messages]{};
    usize free_message_count_{};
    usize relation_count_{};
    usize waiters_{};
    bool opened_{};
    bool closing_{};
    object::ObjectCleanup cleanup_{};
};

} // namespace kernel::ipc
