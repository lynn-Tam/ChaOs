#pragma once

#include <cap/cspace.hpp>
#include <cap/grant.hpp>
#include <cap/resolved.hpp>
#include <core/types.hpp>
#include <ipc/tunnel_link.hpp>
#include <libk/expected.hpp>
#include <libk/noncopyable.hpp>
#include <libk/sync/atomic.hpp>
#include <libk/sync/ticket_spin_lock.hpp>
#include <object/object_cleanup.hpp>
#include <object/object_ref.hpp>

namespace kernel {
class CpuRegistry;
class Vproc;
}

namespace kernel::ipc {

enum class TunnelError : u8 {
    Closed,
    Busy,
    AlreadyConnected,
    WrongSource,
    WrongTarget,
    Empty,
    BadSequence,
    InvalidSlot,
    GenerationExhausted,
    ResourceExhausted,
};

struct TunnelAck final {
    u64 sequence{};
    bool reasserted{};
};

// Receiver-opened, one-shot Vproc virtual interrupt. The same object owns the
// listening offer, connected identity and pending signal truth; Vproc ingress
// state and the event page are projections only.
class Tunnel final : private libk::noncopyable_nonmovable {
    struct AuthorityLink final : private libk::noncopyable_nonmovable {
        AuthorityLink(Tunnel& owner, const cap::GrantAttachmentOps& ops) noexcept
            : owner(&owner), attachment(this, ops) {}

        Tunnel* owner{};
        cap::GrantAttachment attachment;
        cap::GrantWork work{};
    };

public:
    Tunnel(
        CpuRegistry& cpus,
        object::ObjectHold<Vproc>&& target,
        usize slot,
        usize tag) noexcept;
    ~Tunnel() noexcept;

    [[nodiscard]] auto open() noexcept -> libk::Expected<void, TunnelError>;
    [[nodiscard]] auto connect(
        Vproc& source,
        cap::CSpace& cspace,
        const cap::Resolved<Tunnel>& authority) noexcept
        -> libk::Expected<cap::CapHandle, TunnelError>;
    [[nodiscard]] auto invoke(Vproc& caller) noexcept
        -> libk::Expected<u64, TunnelError>;
    [[nodiscard]] auto ack(Vproc& caller, u64 observed) noexcept
        -> libk::Expected<TunnelAck, TunnelError>;
    [[nodiscard]] auto close_from(Vproc& caller, cap::Rights rights) noexcept
        -> libk::Expected<void, TunnelError>;
    void close() noexcept;
    void retire(object::ObjectCleanup&& cleanup) noexcept;

    void retain_relation() noexcept;
    void release_relation() noexcept;
    void peer_stopped(Vproc& peer) noexcept;

private:
    enum class State : u8 {
        Constructing,
        Listening,
        Connecting,
        Idle,
        Pending,
        Closing,
        Closed,
    };

    static void invalidate(
        void* context,
        cap::GrantWork&& work,
        cap::GrantInvalidation reason) noexcept;
    static void released(void* context) noexcept;
    void invalidated(AuthorityLink& link, cap::GrantWork&& work) noexcept;
    void try_finish_close() noexcept;
    void try_finish_retire() noexcept;

    static const cap::GrantAttachmentOps authority_ops_;

    CpuRegistry* cpus_{};
    Vproc* source_{};
    Vproc* target_{};
    object::ObjectHold<Vproc> source_hold_{};
    object::ObjectHold<Vproc> target_hold_{};
    usize slot_{};
    usize tag_{};
    u64 binding_generation_{};
    mutable libk::TicketSpinLock lock_{};
    AuthorityLink connect_authority_;
    TunnelLink source_link_;
    TunnelLink target_link_;
    libk::Atomic<usize> relations_{};
    object::ObjectCleanup cleanup_{};
    u64 claim_generation_{};
    u64 signal_sequence_{};
    bool close_draining_{};
    State state_{State::Constructing};
};

} // namespace kernel::ipc
