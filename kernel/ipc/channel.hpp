#pragma once

#include <cap/cspace.hpp>
#include <cap/grant.hpp>
#include <libk/types.hpp>
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

namespace ipc {

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
    GenerationExhausted,
    Canceled,
    Abandoned,
    InvalidCap,
};

struct ChannelConfig final {
    usize queue_capacity{16};
    usize max_words{CHANNEL_MAX_WORDS};
    usize max_caps{CHANNEL_MAX_CAPS};
};

// A bounded bidirectional queue. Channel owns all queue cells, capability
// escrow and protocol-close state. Endpoint readiness has independent
// read/write sources; subscriptions belong to the receiving Port.
class Channel final : private libk::noncopyable_nonmovable {
public:
    void bind_work(WorkQueue& work, object::Key key) noexcept {
        for (auto& side : sides_) { side.rx.bind_work(work, key); side.tx.bind_work(work, key); }
    }
    template<class Transaction> auto reassign_usage(const object::ref<>& to, Transaction&& transaction) noexcept
        -> std::expected<u64, status_t> {
        { sync::Lock guard{lock_}; if (!opened_) return std::unexpected(STATUS_BUSY); }
        object::ref<> quota{to};
        return transaction([&](auto&& visit) { messages_.charges(visit); },
            [&] { std::swap(quota_, quota); });
    }

private:
    struct Escrow final : private libk::noncopyable {
        Escrow() noexcept = default;
        Escrow(Escrow&&) noexcept = default;
        auto operator=(Escrow&&) noexcept -> Escrow& = default;

        cap::CSpace::Reservation source_slot{};
        cap::Capability cap{};
    };

    struct Message final : private libk::noncopyable {
        libk::intrusive_list_hook hook{};
        ChannelSide destination{ChannelSide::A};
        bool called{};
        u64 transaction{};
        u64 tag{};
        u64 sender_badge{};
        u64 sequence{};
        std::optional<ChannelError> error;
        usize word_count{};
        u64 words[CHANNEL_MAX_WORDS]{};
        libk::inplace_vector<Escrow, CHANNEL_MAX_CAPS> escrows{};
    };

    struct SideLink final : private libk::noncopyable_nonmovable {
        SideLink(Channel& owner, ChannelSide value) noexcept;
        ~SideLink() noexcept = default;

        Channel* owner{};
        ChannelSide side{ChannelSide::A};
        cap::Attachment attachment;
        object::LinkWork work{};
    };

    using MessageQueue = libk::intrusive_list<Message, &Message::hook>;
    struct Side final {
        explicit Side(Channel& owner) noexcept
            : rx(Source::Drained::bind<&Channel::try_finish_retire>(owner)),
              tx(Source::Drained::bind<&Channel::try_finish_retire>(owner)) { tx.update(SOURCE_READY); }
        Src<SOURCE_READY | SOURCE_TERMINAL> rx, tx;
        mutable sync::Spin lock;
        MessageQueue queue{}, free;
        usize occupied{}; // Queued, preparing, or finishing a receive.
        u64 sequence{}, generation{};
        Message* call{};
        bool call_done{};
        u64 readable{};
        std::atomic<u64> writable{SOURCE_READY};
        bool sender_closed{}, receiver_closed{};
    };

public:
    [[nodiscard]] static auto storage_bytes(ChannelConfig config) noexcept -> std::optional<usize>;
    Channel(mm::Pmm& pmm, ChannelConfig config = {}) noexcept;
    ~Channel() noexcept;

    [[nodiscard]] auto open() noexcept -> std::expected<void, ChannelError>;
    [[nodiscard]] auto send(
        cap::Resolved<Channel>& cap,
        cap::CSpace& source,
        const ChanMsg& request) noexcept
        -> std::expected<u64, ChannelError>;
    auto call(cap::Resolved<Channel>&, cap::CSpace&, const ChanMsg&, cap::Resolved<Channel>&) noexcept -> std::expected<u64, ChannelError>;
    auto cancel(cap::Resolved<Channel>&, u64 token, bool discard) noexcept -> bool;
    [[nodiscard]] auto receive(
        cap::Resolved<Channel>& cap,
        cap::CSpace& destination,
        ChanMsg& result) noexcept
        -> std::expected<void, ChannelError>;
    [[nodiscard]] auto close(
        cap::Resolved<Channel>& cap) noexcept
        -> std::expected<void, ChannelError>;
    [[nodiscard]] auto close(ChannelSide side) noexcept -> bool;

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

    void bind_allocation(resource::Allocation& source) noexcept;
    void retire(object::cleanup&& cleanup) noexcept;

    [[nodiscard]] auto side_state(ChannelSide side) const noexcept
        -> bool;
    auto events(ChannelSide side, bool write) noexcept -> Source& {
        return write ? this->side(peer(side)).tx : this->side(side).rx;
    }
    [[nodiscard]] auto config() const noexcept -> ChannelConfig {
        return config_;
    }

private:
    class Flight {
        Channel& owner_;
    public:
        explicit Flight(Channel& owner) noexcept : owner_(owner) { sync::Lock guard{owner.lock_}; ++owner.calls_; }
        ~Flight() noexcept { owner_.finish_call(); }
    };
    void finish_call() noexcept;
    static void invalidate_side(
        void* context,
        object::LinkWork&& work) noexcept;
    static void release_side(void* context, object::LinkWork&&) noexcept;
    void side_invalidated(SideLink& link, object::LinkWork&& work) noexcept;
    void side_released(SideLink&, object::LinkWork&&) noexcept;
    void detach_side(SideLink& link) noexcept;
    auto take_cleanup() noexcept -> object::cleanup;
    void try_finish_retire() noexcept;
    [[nodiscard]] auto side(ChannelSide value) noexcept -> Side&;
    [[nodiscard]] auto side(ChannelSide value) const noexcept -> const Side&;
    [[nodiscard]] auto side_index(ChannelSide value) const noexcept -> usize;
    [[nodiscard]] auto peer(ChannelSide value) const noexcept -> ChannelSide;
    [[nodiscard]] auto side_for(
        const cap::Resolved<Channel>& cap,
        cap::Right right) const noexcept
        -> std::optional<ChannelSide>;
    void readiness_locked(Side&, std::span<Source::Publication, 2>) noexcept;
    void discard_message(Message& message, bool committed = false) noexcept;
    void discard_escrows(std::span<Escrow>, bool committed = false) noexcept;
    void release_message(Message& message) noexcept;
    void clear_queues() noexcept;
    [[nodiscard]] auto send_caps(cap::Resolved<Channel>&, cap::CSpace&, const ChanMsg&, cap::Capability* = nullptr) noexcept -> std::expected<u64, ChannelError>;
    [[nodiscard]] auto enqueue(cap::Resolved<Channel>&, const ChanMsg&, std::span<Escrow>, bool called) noexcept -> std::expected<u64, ChannelError>;
    auto reply(cap::Resolved<Channel>&, const ChanMsg&, std::span<Escrow>) noexcept -> std::expected<u64, ChannelError>;
    auto complete(u64 token, ChannelError, bool discard = false) noexcept -> bool;
    static void abandon(cap::Capability::Facet&) noexcept;
    [[nodiscard]] auto make_escrow(
        cap::CSpace& source,
        const CapXfer& spec,
        Escrow& escrow) noexcept -> std::expected<void, ChannelError>;
    [[nodiscard]] auto commit_escrows(
        Message& message,
        cap::CSpace& destination,
        libk::inplace_vector<cap::CSpace::Reservation,
            CHANNEL_MAX_CAPS>& reservations,
        ChanMsg& result, cap::Graph::Admission,
        std::span<const object::ref<>*> gates, Source::Action commit) noexcept
        -> std::expected<void, ChannelError>;

    static const cap::AttachOps side_ops_;

    ChannelConfig config_{};
    object::ref<> quota_{};
    mm::Slab<Message, false> messages_;
    mutable sync::Spin lock_{};
    Side sides_[2];
    SideLink side_links_[2];
    usize calls_{};
    std::atomic<bool> opened_{}, closing_{};
    object::cleanup cleanup_{};
};

} // namespace ipc
