#pragma once

#include <arch/interrupt.hpp>
#include <cap/authority.hpp>
#include <cap/grant_graph.hpp>
#include <cap/handle.hpp>
#include <cap/policy.hpp>
#include <cap/resolved.hpp>
#include <core/debug.hpp>
#include <libk/expected.hpp>
#include <libk/noncopyable.hpp>
#include <libk/sync/ticket_spin_lock.hpp>
#include <mm/pmm.hpp>
#include <object/object_traits.hpp>
#include <resource/sponsorship.hpp>

namespace kernel::mm {
class VSpace;
}

namespace kernel {
class CpuRegistry;
class ExecutionBinding;
class Thread;
namespace ipc {
class Tunnel;
}
namespace object {
template<typename T>
struct ObjectTraits;
}
}

namespace kernel::cap {

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
};

class CSpace final : private libk::noncopyable_nonmovable {
public:
    struct Quota final {
        usize slots{4096};
        usize pages{130};
    };

    class Reservation final : private libk::noncopyable {
    public:
        Reservation(Reservation&& other) noexcept;
        auto operator=(Reservation&& other) noexcept -> Reservation&;
        ~Reservation() noexcept;

        [[nodiscard]] auto handle() const noexcept -> CapHandle {
            return handle_;
        }

    private:
        friend class CSpace;
        Reservation(
            CSpace& owner,
            CapHandle handle,
            kernel::resource::Reservation&& charge) noexcept
            : owner_(&owner),
              handle_(handle),
              charge_(libk::move(charge)) {}
        void reset() noexcept;
        void disarm() noexcept {
            owner_ = nullptr;
            handle_ = {};
        }

        CSpace* owner_{};
        CapHandle handle_{};
        kernel::resource::Reservation charge_{};
    };

    // Reserves one destination capability slot and one sponsored Grant node
    // as a single pre-publication unit. Type-specific derivation still lives
    // in GrantGraph; this class only owns rollback-safe resource capacity.
    class DerivationReservation final : private libk::noncopyable {
    public:
        DerivationReservation(DerivationReservation&&) noexcept = default;
        auto operator=(DerivationReservation&&) noexcept
            -> DerivationReservation& = default;

        [[nodiscard]] auto handle() const noexcept -> CapHandle {
            return slot_.handle();
        }

    private:
        friend class CSpace;
        friend class kernel::ipc::Tunnel;

        DerivationReservation(
            Reservation&& slot,
            kernel::resource::Reservation&& grant) noexcept
            : slot_(libk::move(slot)), grant_(libk::move(grant)) {}

        Reservation slot_;
        kernel::resource::Reservation grant_;
    };

    explicit CSpace(kernel::mm::Pmm& pmm) noexcept;
    CSpace(kernel::mm::Pmm& pmm, Quota quota) noexcept;
    ~CSpace() noexcept;

    [[nodiscard]] auto reserve() noexcept
        -> libk::Expected<Reservation, CSpaceError>;
    [[nodiscard]] auto reserve_derivation() noexcept
        -> libk::Expected<DerivationReservation, CSpaceError>;
    [[nodiscard]] auto insert(
        GrantRef&& grant,
        CapView view) noexcept -> libk::Expected<CapHandle, CSpaceError>;
    [[nodiscard]] auto insert(
        Reservation&& reservation,
        GrantRef&& grant,
        CapView view) noexcept -> libk::Expected<CapHandle, CSpaceError>;
    [[nodiscard]] auto close(CapHandle handle) noexcept
        -> libk::Expected<void, CSpaceError>;
    [[nodiscard]] auto duplicate(
        CapHandle source,
        CSpace& destination,
        CapView view) noexcept -> libk::Expected<CapHandle, CSpaceError>;
    [[nodiscard]] auto duplicate(
        CapHandle source,
        CSpace& destination,
        Rights rights) noexcept -> libk::Expected<CapHandle, CSpaceError>;
    [[nodiscard]] auto delegate(
        CapHandle source,
        CSpace& destination,
        GrantCeiling ceiling,
        CapView view) noexcept -> libk::Expected<CapHandle, CSpaceError>;
    [[nodiscard]] auto delegate(
        CapHandle source,
        CSpace& destination,
        Rights rights) noexcept -> libk::Expected<CapHandle, CSpaceError>;
    [[nodiscard]] auto move(
        CapHandle source,
        CSpace& destination) noexcept -> libk::Expected<CapHandle, CSpaceError>;
    [[nodiscard]] auto revoke(
        CapHandle source,
        GrantRevoke& completion,
        bool include_source) noexcept -> libk::Expected<void, CSpaceError>;
    [[nodiscard]] auto revoke(
        CapHandle source,
        Thread& thread,
        CpuRegistry& cpus,
        bool include_source) noexcept
        -> libk::Expected<kernel::operation::State, CSpaceError>;
    [[nodiscard]] auto destroy(CapHandle source) noexcept
        -> libk::Expected<void, CSpaceError>;

    template<typename T>
    [[nodiscard]] auto resolve(
        CapHandle handle,
        Rights requested) noexcept -> libk::Expected<Resolved<T>, CSpaceError> {
        static_assert(object::StorableObject<T>);
        auto copied = snapshot(handle);
        if (!copied) {
            return libk::unexpected(copied.error());
        }
        Snapshot source = copied.value();
        auto acquired = source.graph->acquire(source.key);
        if (!acquired) {
            return libk::unexpected(CSpaceError::GrantUnavailable);
        }
        GrantLease lease = libk::move(acquired).value();
        if (lease.kind() != object::ObjectTraits<T>::kind) {
            return libk::unexpected(CSpaceError::WrongKind);
        }
        auto effective = cap::compose(
            lease.kind(), lease.ceiling(), source.view);
        if (!effective) {
            return libk::unexpected(policy_error(effective.error()));
        }
        if (!effective.value().rights.contains(requested)) {
            return libk::unexpected(CSpaceError::Denied);
        }
        auto target = lease.clone_target();
        if (!target) {
            return libk::unexpected(CSpaceError::GrantUnavailable);
        }
        auto pin = target.value().template pin<T>();
        if (!pin) {
            return libk::unexpected(CSpaceError::GrantUnavailable);
        }
        return libk::expected(Resolved<T>{
            *this,
            libk::move(pin).value(),
            libk::move(lease),
            effective.value()});
    }

    void retire() noexcept;
    [[nodiscard]] auto binding_count() const noexcept -> usize;
    [[nodiscard]] auto live_slots() const noexcept -> usize;
    [[nodiscard]] auto table_pages() const noexcept -> usize;

private:
    friend class ::kernel::mm::VSpace;
    friend class kernel::ExecutionBinding;
    friend struct kernel::object::ObjectTraits<CSpace>;
    static constexpr usize leaf_bits = 3;
    static constexpr usize dir_bits = 8;
    static constexpr usize leaf_slots = usize{1} << leaf_bits;
    static constexpr usize dir_entries = usize{1} << dir_bits;
    static constexpr u32 invalid_index = ~u32{};
    static constexpr usize max_leaves = dir_entries * dir_entries;

    struct Capability final {
        GrantRef grant{};
        CapView view{};
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
        kernel::resource::Sponsorship sponsorship{};

        ~Slot() noexcept {
            KASSERT(state != SlotState::Occupied);
            KASSERT(!sponsorship);
        }
    };

    struct DirPage final {
        DirPage(
            kernel::mm::Page physical,
            kernel::resource::Reservation&& charge) noexcept
            : page(physical) {
            if (charge) {
                sponsorship.commit(libk::move(charge));
            }
        }

        kernel::mm::Page page{};
        kernel::resource::Sponsorship sponsorship{};
        void* children[dir_entries]{};
    };

    struct LeafPage final {
        LeafPage(
            u32 base,
            kernel::mm::Page physical,
            kernel::resource::Reservation&& charge) noexcept
            : base_index(base), page(physical) {
            if (charge) {
                sponsorship.commit(libk::move(charge));
            }
        }
        u32 base_index{};
        u32 padding{};
        kernel::mm::Page page{};
        kernel::resource::Sponsorship sponsorship{};
        Slot slots[leaf_slots]{};
    };

    static_assert(sizeof(DirPage) <= kernel::mm::page_size);
    static_assert(sizeof(LeafPage) <= kernel::mm::page_size);

    struct Snapshot final {
        GrantGraph* graph{};
        GrantKey key{};
        CapView view{};
    };

    [[nodiscard]] auto snapshot(CapHandle handle) noexcept
        -> libk::Expected<Snapshot, CSpaceError>;
    [[nodiscard]] auto commit(
        Reservation& reservation,
        GrantRef&& grant,
        CapView view) noexcept -> libk::Expected<CapHandle, CSpaceError>;
    // Caller holds lock_. Reservation is the operation lease which keeps this
    // prepared slot valid even after retirement closes new admission.
    [[nodiscard]] auto commit_locked(
        Reservation& reservation,
        GrantRef&& grant,
        CapView view) noexcept -> libk::Expected<CapHandle, CSpaceError>;
    void rollback(CapHandle handle) noexcept;
    void finish_retire() noexcept;
    [[nodiscard]] auto reserve_grant() noexcept
        -> libk::Expected<kernel::resource::Reservation, CSpaceError>;
    [[nodiscard]] auto prepare_retire() noexcept -> bool;
    [[nodiscard]] auto attach_execution() noexcept -> bool;
    void detach_execution() noexcept;
    void bind_sponsor(kernel::resource::Sponsorship& sponsor) noexcept;
    [[nodiscard]] auto grow() noexcept -> libk::Expected<void, CSpaceError>;
    [[nodiscard]] auto slot(usize index) noexcept -> Slot*;
    [[nodiscard]] auto slot(usize index) const noexcept -> const Slot*;
    void push_free(usize index, Slot& slot) noexcept;
    void link_occupied(usize index, Slot& slot) noexcept;
    void unlink_occupied(usize index, Slot& slot) noexcept;
    [[nodiscard]] static auto policy_error(PolicyError error) noexcept
        -> CSpaceError;
    [[nodiscard]] static auto grant_error(GrantError error) noexcept
        -> CSpaceError;

    kernel::mm::Pmm* pmm_{};
    kernel::mm::OwnedPageGroup pages_;
    Quota quota_{};
    mutable libk::TicketSpinLock lock_{};
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
    kernel::resource::Sponsorship* sponsor_{};
};

} // namespace kernel::cap
