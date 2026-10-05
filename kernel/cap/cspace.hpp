#pragma once

#include <utility>


#include <bit>

#include <arch/interrupt.hpp>
#include <cap/cap.hpp>
#include <cap/grant.hpp>
#include <libk/assert.hpp>
#include <base/types.hpp>
#include <expected>
#include <libk/noncopyable.hpp>
#include <span>
#include <sync.hpp>
#include <mm/pmm.hpp>
#include <resource/sponsorship.hpp>

namespace mm {
class VSpace;
}

class Env;

namespace ipc {
class Transfer;
class Channel;
}
namespace object {
template<typename T>
struct traits;
}

namespace cap {

enum class CSpaceError : u8 {
    InvalidHandle,
    InvalidState,
    WrongKind,
    Denied,
    Amplification,
    OutOfMemory,
    SlotQuota,
    PageQuota,
    GenerationExhausted,
    Contended,
    GrantUnavailable,
    ResourceExhausted,
    InvalidDescriptor,
};

class CSpace final : private libk::noncopyable_nonmovable {
public:
    struct Quota final {
        usize slots{4096};
        usize pages{130};
    };

    class Reservation final : private libk::noncopyable {
    public:
        Reservation() noexcept = default;
        Reservation(Reservation&& other) noexcept;
        auto operator=(Reservation&& other) noexcept -> Reservation&;
        ~Reservation() noexcept;

        [[nodiscard]] auto handle() const noexcept -> Handle {
            return handle_;
        }

    private:
        friend class CSpace;
        friend class ipc::Transfer;
        Reservation(
            CSpace& owner,
            Handle handle,
            resource::Charge&& charge) noexcept
            : owner_(&owner),
              handle_(handle),
              charge_(std::move(charge)) {}
        void reset() noexcept;
        void disarm() noexcept {
            owner_ = nullptr;
            handle_ = {};
        }

        CSpace* owner_{};
        Handle handle_{};
        resource::Charge charge_{};
    };

    // Validated admission and reserved storage; no capability is visible yet.
    class NewCap {
    public:
        NewCap() noexcept = default;
        NewCap(NewCap&&) noexcept = default;
        auto operator=(NewCap&&) noexcept -> NewCap& = default;
        auto handle() const noexcept -> Handle { return slot_.handle(); }

    private:
        friend class CSpace;
        NewCap(Reservation&& slot, GrantRef&& grant, GrantLease&& pin, View view) noexcept
            : slot_(std::move(slot)), grant_(std::move(grant)), pin_(std::move(pin)), view_(view) {}
        Reservation slot_;
        GrantRef grant_;
        GrantLease pin_;
        View view_{};
    };

    // Reserves one destination capability slot and one sponsored Grant node
    // as a single pre-publication unit. Type-specific derivation still lives
    // in GrantGraph; this class only owns rollback-safe resource capacity.
    class DerivationReservation final : private libk::noncopyable {
    public:
        DerivationReservation(DerivationReservation&&) noexcept = default;
        auto operator=(DerivationReservation&&) noexcept
            -> DerivationReservation& = default;

        [[nodiscard]] auto handle() const noexcept -> Handle {
            return slot_.handle();
        }

    private:
        friend class CSpace;
        friend class ipc::Channel;

        DerivationReservation(
            Reservation&& slot,
            resource::Reservation&& grant) noexcept
            : slot_(std::move(slot)), grant_(std::move(grant)) {}

        Reservation slot_;
        resource::Reservation grant_;
    };

    explicit CSpace(mm::Pmm& pmm) noexcept;
    CSpace(mm::Pmm& pmm, Quota quota) noexcept;
    ~CSpace() noexcept;

    [[nodiscard]] auto reserve() noexcept
        -> std::expected<Reservation, CSpaceError>;
    [[nodiscard]] auto reserve_derivation() noexcept
        -> std::expected<DerivationReservation, CSpaceError>;
    [[nodiscard]] auto insert(
        GrantRef&& grant,
        View view) noexcept -> std::expected<Handle, CSpaceError>;
    [[nodiscard]] auto insert(
        Reservation&& reservation,
        GrantRef&& grant,
        View view) noexcept -> std::expected<Handle, CSpaceError>;
    [[nodiscard]] auto prepare(Reservation&&, GrantRef&&, View) noexcept
        -> std::expected<NewCap, CSpaceError>;
    // All slots become visible under one lock, or none do. No allocation here.
    [[nodiscard]] auto insert(std::span<NewCap>) noexcept
        -> std::expected<void, CSpaceError>;
    [[nodiscard]] auto close(Handle handle) noexcept
        -> std::expected<void, CSpaceError>;
    [[nodiscard]] auto duplicate(
        Handle source,
        CSpace& destination,
        View view) noexcept -> std::expected<Handle, CSpaceError>;
    [[nodiscard]] auto duplicate(
        Handle source,
        CSpace& destination,
        Rights rights) noexcept -> std::expected<Handle, CSpaceError>;
    [[nodiscard]] auto delegate(
        Handle source,
        CSpace& destination,
        View ceiling,
        View view) noexcept -> std::expected<Handle, CSpaceError>;
    [[nodiscard]] auto delegate(
        Handle source,
        CSpace& destination,
        Rights rights) noexcept -> std::expected<Handle, CSpaceError>;
    [[nodiscard]] auto typed_delegate(
        Handle source,
        CSpace& destination,
        const Attenuation& descriptor) noexcept
        -> std::expected<Handle, CSpaceError>;
    [[nodiscard]] auto move(
        Handle source,
        CSpace& destination) noexcept -> std::expected<Handle, CSpaceError>;
    [[nodiscard]] auto revoke(
        Handle source,
        GrantRevoke& completion,
        bool include_source) noexcept -> std::expected<void, CSpaceError>;
    [[nodiscard]] auto destroy(Handle source) noexcept
        -> std::expected<void, CSpaceError>;

    template<typename T>
    [[nodiscard]] auto resolve(
        Handle handle,
        Rights requested) noexcept -> std::expected<Resolved<T>, CSpaceError> {
        static_assert(object::kind<T> != object::ObjectKind::Invalid);
        auto copied = snapshot(handle);
        if (!copied) {
            return std::unexpected(copied.error());
        }
        Snapshot source = std::move(copied).value();
        GrantLease lease = std::move(source.lease);
        if (lease.kind() != object::kind<T>) {
            return std::unexpected(CSpaceError::WrongKind);
        }
        if (!source.view.rights.contains(requested)) {
            return std::unexpected(CSpaceError::Denied);
        }
        if (!lease.target_live()) return std::unexpected(CSpaceError::GrantUnavailable);
        return (Resolved<T>{
            *this,
            std::move(lease),
            source.view});
    }

    void retire() noexcept;
    [[nodiscard]] auto binding_count() const noexcept -> usize;
    [[nodiscard]] auto live_slots() const noexcept -> usize;
    [[nodiscard]] auto table_pages() const noexcept -> usize;

private:
    friend class ::mm::VSpace;
    friend class ::Env;
    friend class ipc::Transfer;
    friend class ipc::Channel;
    friend struct object::traits<CSpace>;
    static constexpr usize dir_bits = 8;
    static constexpr usize dir_entries = usize{1} << dir_bits;
    static constexpr u32 invalid_index = ~u32{};
    static constexpr usize max_leaves = dir_entries * dir_entries;

    struct Capability final {
        GrantRef grant{};
        View view{};
    };

    enum class SlotState : u8 {
        Empty,
        Reserved,
        Occupied,
        Quarantined,
    };

    struct Slot final {
        union Storage {
            byte empty;
            Capability capability;

            constexpr Storage() noexcept : empty{} {}
            ~Storage() {}
        } storage{};
        u64 generation{};
        u32 next{invalid_index};
        u32 previous{invalid_index};
        SlotState state{SlotState::Empty};

        ~Slot() noexcept {
            libk_assert(state != SlotState::Occupied);
        }
    };

    struct DirPage final {
        explicit DirPage(mm::Page physical) noexcept : page(physical) {}
        mm::Page page{};
        void* children[dir_entries]{};
    };

    static constexpr usize leaf_bits = std::bit_width(
        (mm::page_size - sizeof(mm::Page)) / sizeof(Slot)) - 1;
    static constexpr usize leaf_slots = usize{1} << leaf_bits;

    struct LeafPage final {
        explicit LeafPage(mm::Page physical) noexcept : page(physical) {}
        mm::Page page{};
        Slot slots[leaf_slots]{};
    };

    static_assert(sizeof(DirPage) <= mm::page_size);
    static_assert(sizeof(LeafPage) <= mm::page_size);

    // The slot view is validated once while acquiring its grant admission.
    struct Snapshot final {
        GrantLease lease;
        View view;
    };

    [[nodiscard]] auto snapshot(Handle handle) noexcept
        -> std::expected<Snapshot, CSpaceError>;
    [[nodiscard]] auto duplicate_snapshot(Snapshot&&, CSpace&, View) noexcept
        -> std::expected<Handle, CSpaceError>;
    [[nodiscard]] auto delegate_snapshot(
        Snapshot&& source,
        CSpace& destination,
        View ceiling,
        View view) noexcept
        -> std::expected<Handle, CSpaceError>;
    [[nodiscard]] auto commit(
        Reservation& reservation,
        GrantRef&& grant,
        View view) noexcept -> std::expected<Handle, CSpaceError>;
    // Caller holds lock_. Reservation is the operation lease which keeps this
    // prepared slot valid even after retirement closes new admission.
    [[nodiscard]] auto commit_locked(
        Reservation& reservation,
        GrantRef&& grant,
        View view) noexcept -> std::expected<Handle, CSpaceError>;
    [[nodiscard]] auto reserved(const Reservation&) noexcept -> Slot*;
    void publish(Reservation&, Capability&&) noexcept;
    void rollback(Handle handle) noexcept;
    void finish_retire() noexcept;
    [[nodiscard]] auto reserve_grant() noexcept
        -> std::expected<resource::Reservation, CSpaceError>;
    [[nodiscard]] auto prepare_retire() noexcept -> bool;
    [[nodiscard]] auto escrow_move(
        Handle source,
        GrantRef& grant,
        View& view,
        Reservation& reservation) noexcept
        -> std::expected<void, CSpaceError>;
    [[nodiscard]] auto escrow_restore(
        Reservation& reservation,
        GrantRef&& grant,
        View view) noexcept -> bool;
    [[nodiscard]] auto escrow_drop(
        Reservation& reservation) noexcept -> resource::Charge;
    void retain_escrow() noexcept;
    void release_escrow() noexcept;
    [[nodiscard]] auto attach_execution() noexcept -> bool;
    void detach_execution() noexcept;
    void bind_sponsor(resource::Sponsorship& sponsor) noexcept;
    [[nodiscard]] auto grow() noexcept -> std::expected<void, CSpaceError>;
    [[nodiscard]] auto slot(usize index) noexcept -> Slot*;
    [[nodiscard]] auto slot(usize index) const noexcept -> const Slot*;
    void push_free(usize index, Slot& slot) noexcept;
    void link_occupied(usize index, Slot& slot) noexcept;
    void unlink_occupied(usize index, Slot& slot) noexcept;
    [[nodiscard]] static auto policy_error(PolicyError error) noexcept
        -> CSpaceError;
    [[nodiscard]] static auto grant_error(GrantError error) noexcept
        -> CSpaceError;

    mm::Pmm* pmm_{};
    mm::PageGroup pages_;
    Quota quota_{};
    mutable sync::Spin lock_{};
    DirPage* root_{};
    u32 free_head_{invalid_index};
    u32 occupied_head_{invalid_index};
    usize next_leaf_{};
    usize page_count_{};
    usize live_slots_{};
    usize quarantined_slots_{};
    bool accepting_{true};
    bool growing_{};
    bool releasing_{};
    bool retired_{};
    usize bindings_{};
    usize escrows_{};
    resource::Sponsorship* sponsor_{};
    // All table pages and committed selectors share this immutable sponsor.
    // Detached subcharges refund only after the corresponding storage is free.
    resource::Charge charge_{};
};

} // namespace cap
