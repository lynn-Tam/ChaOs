#pragma once

#include <cap/cspace.hpp>
#include <cap/grant.hpp>
#include <base/types.hpp>
#include <ipc/notification.hpp>
#include <expected>
#include <libk/inplace_vector.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/noncopyable.hpp>
#include <optional>
#include <mm/pmm.hpp>
#include <object/ref.hpp>
#include <wait.hpp>
#include <sync.hpp>
#include <uapi/ipc.h>

class Cpus;
class Thread;

namespace ipc {

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
    Canceled,
    InvalidCap,
};

enum class ChannelCondition : u8 {
    Readable = CHANNEL_READABLE,
    Writable = CHANNEL_WRITABLE,
    PeerClosed = CHANNEL_PEER_CLOSED,
};

struct ChannelConfig final {
    usize queue_capacity{16};
    usize max_words{CHANNEL_MAX_WORDS};
    usize max_caps{CHANNEL_MAX_CAPS};
    usize relation_capacity{4};
};

struct ChannelSend final {
    u64 transaction{};
    u64 tag{};
    usize word_count{};
    u64 words[CHANNEL_MAX_WORDS]{};
    usize cap_count{};
    CapXfer caps[CHANNEL_MAX_CAPS]{};
};

struct ChannelRecv final {
    u64 transaction{};
    u64 tag{};
    u64 sender_badge{};
    u64 sequence{};
    usize word_count{};
    u64 words[CHANNEL_MAX_WORDS]{};
    usize cap_count{};
    usize receive_limit{CHANNEL_MAX_CAPS};
    cap::Handle caps[CHANNEL_MAX_CAPS]{};
};

// A bounded bidirectional queue. Channel owns all queue cells, capability
// escrow, readiness relations, and protocol-close state; Notification only
// receives level/sequence hints through NotificationSource.
class Channel final : private libk::noncopyable_nonmovable {
public:
    struct Wait final : private libk::noncopyable_nonmovable {
        enum class Kind : u8 {
            Send,
            Receive,
        };
        enum class State : u8 {
            Attaching,
            Awaiting,
            Armed,
            Ready,
            Done,
        };

        explicit Wait(Channel& owner) noexcept;
        ~Wait() noexcept;


        void release() noexcept;
        [[nodiscard]] auto cancel() noexcept -> bool;

        Channel* owner{};
        Kind kind{Kind::Send};
        State state{State::Attaching};
        libk::IntrusiveListHook hook{};
        // Admission, Completion, cap attachment, and lock-external
        // notifications each own one reference. Protected by Channel::lock_.
        usize references{1};
        bool admitted{};
        bool grant_detaching{};
        ChannelSide side{ChannelSide::A};
        WaitResult result{};
        object::ref<> channel_ref{};
        cap::GrantAttachment grant_attachment;
        Completion completion;
    };

private:
    using Waiter = Wait;
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
        cap::View view{};
        cap::Handle source_handle{};
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
        u64 words[CHANNEL_MAX_WORDS]{};
        libk::InplaceVector<Escrow, CHANNEL_MAX_CAPS> escrows{};
    };

    struct Relation;

    struct GrantLink final : private libk::noncopyable_nonmovable {
        GrantLink(Relation& relation, bool channel) noexcept;
        ~GrantLink() noexcept = default;

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
        object::ref<Notification> notification{};
        GrantLink channel_link;
        GrantLink notification_link;
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

public:
    [[nodiscard]] static auto storage_bytes(ChannelConfig config) noexcept -> std::optional<usize>;
    Channel(mm::Pmm& pmm, ChannelConfig config = {}) noexcept;
    ~Channel() noexcept;

    [[nodiscard]] auto open() noexcept -> std::expected<void, ChannelError>;
    [[nodiscard]] auto send(
        cap::Resolved<Channel>& cap,
        cap::CSpace& source,
        const ChannelSend& request,
        Wait* turn = nullptr) noexcept
        -> std::expected<u64, ChannelError>;
    [[nodiscard]] auto receive(
        cap::Resolved<Channel>& cap,
        cap::CSpace& destination,
        ChannelRecv& result,
        Wait* turn = nullptr) noexcept
        -> std::expected<void, ChannelError>;
    // Reserve the fair queue turn. The caller owns request/result storage and
    // keeps Wait alive through transfer and result publication.
    [[nodiscard]] auto wait(
        cap::Resolved<Channel>&& cap, Wait& waiter, Wait::Kind kind,
        Thread& thread, Cpus& cpus) noexcept
        -> std::expected<void, ChannelError>;
    [[nodiscard]] auto close(
        cap::Resolved<Channel>& cap) noexcept
        -> std::expected<void, ChannelError>;
    [[nodiscard]] auto close(ChannelSide side) noexcept -> bool;

    [[nodiscard]] auto bind(
        cap::Resolved<Channel>& cap,
        cap::Resolved<Notification>& notification,
        ChannelCondition condition) noexcept
        -> std::expected<usize, ChannelError>;
    [[nodiscard]] auto arm(
        cap::Resolved<Channel>& cap,
        usize relation,
        u64 observed) noexcept -> std::expected<u64, ChannelError>;

    [[nodiscard]] auto mint(
        cap::Resolved<Channel>& cap,
        cap::CSpace& destination,
        u64 badge,
        cap::Rights rights) noexcept
        -> std::expected<cap::Handle, ChannelError>;

    // Bound during construction. Revoking the exact side-root grant closes
    // that protocol side through the cap graph's lifecycle edge.
    [[nodiscard]] auto bind_side_root(
        cap::GrantRef& root,
        ChannelSide side) noexcept -> bool;

    void bind_sponsor(resource::Sponsorship& sponsor) noexcept;
    void retire(object::cleanup&& cleanup) noexcept;

    [[nodiscard]] auto side_state(ChannelSide side) const noexcept
        -> bool;
    [[nodiscard]] auto config() const noexcept -> ChannelConfig {
        return config_;
    }

private:
    static void invalidate(
        void* context,
        cap::GrantWork&& work) noexcept;
    static void released(void* context) noexcept;
    void invalidated(GrantLink& link, cap::GrantWork&& work) noexcept;
    void relation_released(Relation& relation) noexcept;
    static void invalidate_side(
        void* context,
        cap::GrantWork&& work) noexcept;
    static void release_side(void* context) noexcept;
    void side_invalidated(SideLink& link, cap::GrantWork&& work) noexcept;
    void side_released(SideLink& link) noexcept;
    static void invalidate_waiter(
        void* context,
        cap::GrantWork&& work) noexcept;
    static void release_waiter(void* context) noexcept;
    void waiter_invalidated(Waiter& waiter, cap::GrantWork&& work) noexcept;
    void drop_waiter(Waiter& waiter) noexcept;
    void detach_waiter_grant(Waiter& waiter) noexcept;
    void finish_waiter(Waiter& waiter) noexcept;
    void abort_relation(Relation& relation) noexcept;
    void detach_side(SideLink& link) noexcept;
    void try_finish_retire() noexcept;
    using WaitQueue = libk::IntrusiveList<Waiter, &Waiter::hook>;
    [[nodiscard]] auto wait_queue(Waiter::Kind kind, ChannelSide side) noexcept -> WaitQueue&;
    [[nodiscard]] auto owns_turn_locked(
        Waiter::Kind kind, ChannelSide side, const Waiter* reservation) noexcept -> bool;
    [[nodiscard]] auto waiter_ready_locked(
        const Waiter& waiter) noexcept -> bool;
    [[nodiscard]] auto arm_waiter(Waiter& waiter) noexcept -> bool;
    [[nodiscard]] auto side(ChannelSide value) noexcept -> Side&;
    [[nodiscard]] auto side(ChannelSide value) const noexcept -> const Side&;
    [[nodiscard]] auto side_index(ChannelSide value) const noexcept -> usize;
    [[nodiscard]] auto peer(ChannelSide value) const noexcept -> ChannelSide;
    [[nodiscard]] auto side_for(
        const cap::Resolved<Channel>& cap,
        cap::Right right) const noexcept
        -> std::optional<ChannelSide>;
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
        const CapXfer& spec,
        Escrow& escrow) noexcept -> std::expected<void, ChannelError>;
    [[nodiscard]] auto commit_escrows(
        Message& message,
        cap::CSpace& destination,
        libk::InplaceVector<cap::CSpace::Reservation,
            CHANNEL_MAX_CAPS>& reservations,
        ChannelRecv& result) noexcept -> CommitResult;

    static const cap::GrantAttachmentOps channel_ops_;
    static const cap::GrantAttachmentOps notification_ops_;
    static const cap::GrantAttachmentOps side_ops_;
    static const cap::GrantAttachmentOps waiter_ops_;

    ChannelConfig config_{};
    object::ref<> payer_{};
    mm::Slab<Message, false> messages_;
    mutable sync::Spin lock_{};
    Side sides_[2]{};
    SideLink side_links_[2];
    mm::Slab<Relation, false> relation_pool_;
    libk::IntrusiveList<Relation, &Relation::hook> relations_{};
    WaitQueue wait_queues_[2][2]{};
    MessageQueue free_messages_{};
    usize waiter_count_{};
    bool opened_{};
    bool closing_{};
    object::cleanup cleanup_{};
};

} // namespace ipc
