#pragma once

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
    WrongSource,
    WrongTarget,
    Empty,
    InvalidSlot,
    GenerationExhausted,
};

// A capability-authorized virtual interrupt between two immutable Vprocs.
// Tunnel owns delivery truth; the target slot and event page are projections.
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
        Vproc& source,
        Vproc& target,
        usize slot,
        usize tag) noexcept;
    ~Tunnel() noexcept;

    [[nodiscard]] auto authorize(
        const cap::Resolved<Vproc>& source,
        const cap::Resolved<Vproc>& target) noexcept
        -> libk::Expected<void, TunnelError>;
    [[nodiscard]] auto invoke(Vproc& caller) noexcept
        -> libk::Expected<u64, TunnelError>;
    [[nodiscard]] auto take(Vproc& caller) noexcept
        -> libk::Expected<u64, TunnelError>;
    void close() noexcept;
    void retire(object::ObjectCleanup&& cleanup) noexcept;

    // Vproc holds no owning pointer to Tunnel. A linked relation may acquire a
    // short callback lease while holding its own lock; retirement detaches the
    // relation before waiting for these leases.
    void retain_relation() noexcept;
    void release_relation() noexcept;
    void peer_stopped(Vproc& peer) noexcept;

private:
    enum class State : u8 {
        Constructing,
        Open,
        Closing,
        Closed,
    };

    static void invalidate(
        void* context,
        cap::GrantWork&& work,
        cap::GrantInvalidation reason) noexcept;
    static void released(void* context) noexcept;
    void invalidated(AuthorityLink& link, cap::GrantWork&& work) noexcept;
    void finish_close() noexcept;
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
    AuthorityLink source_authority_;
    AuthorityLink target_authority_;
    TunnelLink source_link_;
    TunnelLink target_link_;
    libk::Atomic<usize> relations_{};
    object::ObjectCleanup cleanup_{};
    u64 delivery_generation_{};
    bool pending_{};
    State state_{State::Constructing};
};

} // namespace kernel::ipc
