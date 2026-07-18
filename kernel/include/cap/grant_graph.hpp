#pragma once

#include <cap/grant.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/noncopyable.hpp>
#include <libk/sync/ticket_spin_lock.hpp>
#include <mm/node_pool.hpp>
#include <mm/pmm.hpp>
#include <resource/allocation.hpp>
#include <resource/sponsorship.hpp>

namespace kernel {
class CpuRegistry;
class Thread;
}

namespace kernel::cap {

class CSpace;

class GrantGraph final : private libk::noncopyable_nonmovable {
public:
    struct Quota final {
        usize nodes{4096};
        usize pages{64};
    };

    explicit GrantGraph(kernel::mm::Pmm& pmm) noexcept;
    GrantGraph(kernel::mm::Pmm& pmm, Quota quota) noexcept;
    ~GrantGraph() noexcept;

    [[nodiscard]] auto create_root(
        object::ObjectRef&& target,
        GrantCeiling ceiling) noexcept -> libk::Expected<GrantRef, GrantError>;
    [[nodiscard]] auto create_root(
        kernel::resource::Reservation&& charge,
        object::ObjectRef&& target,
        GrantCeiling ceiling) noexcept -> libk::Expected<GrantRef, GrantError>;

    [[nodiscard]] auto create_allocation(
        kernel::resource::Permit& permit,
        kernel::resource::Reservation&& charge,
        object::ObjectRef&& target,
        GrantCeiling ceiling) noexcept
        -> libk::Expected<kernel::resource::AllocationTxn, GrantError>;

    [[nodiscard]] auto derive(
        const GrantLease& source,
        object::ObjectRef&& target,
        GrantCeiling ceiling) noexcept -> libk::Expected<GrantRef, GrantError>;
    [[nodiscard]] auto derive(
        kernel::resource::Reservation&& charge,
        const GrantLease& source,
        object::ObjectRef&& target,
        GrantCeiling ceiling) noexcept -> libk::Expected<GrantRef, GrantError>;

    [[nodiscard]] auto derive_region(
        kernel::resource::Reservation&& charge,
        const GrantLease& source,
        object::ObjectRef&& target,
        GrantCeiling ceiling,
        RegionDerivation proof) noexcept
        -> libk::Expected<GrantRef, GrantError>;

    [[nodiscard]] auto ref(GrantKey key) noexcept
        -> libk::Expected<GrantRef, GrantError>;
    [[nodiscard]] auto acquire(GrantKey key) noexcept
        -> libk::Expected<GrantLease, GrantError>;
    [[nodiscard]] auto attach(
        const GrantLease& source,
        GrantAttachment& attachment) noexcept
        -> libk::Expected<void, GrantError>;

    [[nodiscard]] auto revoke_descendants(
        GrantKey source,
        GrantRevoke& completion) noexcept -> libk::Expected<void, GrantError>;
    [[nodiscard]] auto invalidate(
        GrantKey source,
        GrantRevoke& completion) noexcept -> libk::Expected<void, GrantError>;

    [[nodiscard]] auto state(GrantKey key) const noexcept
        -> libk::Expected<GrantState, GrantError>;
    [[nodiscard]] auto live_count() const noexcept -> usize;
    [[nodiscard]] auto close_pool(
        kernel::resource::ResourcePool& pool,
        const kernel::object::ObjectRef& self,
        kernel::Thread& thread,
        kernel::CpuRegistry& cpus) noexcept
        -> libk::Expected<kernel::operation::State, GrantError>;
    [[nodiscard]] static auto node_charge() noexcept
        -> kernel::resource::Budget;

private:
    friend class CSpace;
    friend class GrantRef;
    friend class GrantLease;
    friend class GrantAttachment;
    friend class GrantRevokeWait;
    friend class kernel::resource::CloseWait;
    friend class kernel::resource::AllocationTxn;
    friend class kernel::resource::ResourcePool;

    struct Slot;

    struct Node final {
        libk::IntrusiveListHook child_hook{};
        using ChildList = libk::IntrusiveList<Node, &Node::child_hook>;
        using AttachmentList = libk::IntrusiveList<
            GrantAttachment,
            &GrantAttachment::grant_hook_>;

        Node(
            Slot& owner,
            object::ObjectRef&& target_ref,
            GrantCeiling authority,
            Node* parent_node,
            kernel::resource::Reservation&& charge) noexcept
            : slot(&owner),
              target(libk::move(target_ref)),
              ceiling(authority),
              parent(parent_node) {
            if (charge) {
                sponsorship.commit(libk::move(charge));
            }
        }

        Slot* slot{};
        object::ObjectRef target{};
        GrantCeiling ceiling{};
        Node* parent{};
        ChildList children{};
        AttachmentList attachments{};
        GrantRevoke* revoke{};
        usize refs{};
        usize operations{};
        GrantState state{GrantState::Live};
        bool reclaiming{};
        kernel::resource::Sponsorship sponsorship{};
        kernel::resource::Allocation allocation{};
    };

    struct PageHeader;
    struct Slot final {
        PageHeader* page{};
        Slot* next_free{};
        u64 generation{};
        bool occupied{};
        bool quarantined{};
        alignas(Node) byte storage[sizeof(Node)]{};

        [[nodiscard]] auto node() noexcept -> Node* {
            return reinterpret_cast<Node*>(storage);
        }
        [[nodiscard]] auto node() const noexcept -> const Node* {
            return reinterpret_cast<const Node*>(storage);
        }
    };

    struct PageHeader final {
        explicit PageHeader(kernel::mm::OwnedPage&& page) noexcept
            : backing(libk::move(page)) {}

        kernel::mm::OwnedPage backing;
        PageHeader* next{};
        Slot* free_head{};
        usize free_count{};
        usize live_count{};
    };

    static constexpr usize slot_offset =
        (sizeof(PageHeader) + alignof(Slot) - 1) & ~(alignof(Slot) - 1);
    static constexpr usize slots_per_page =
        (kernel::mm::page_size - slot_offset) / sizeof(Slot);

    [[nodiscard]] auto create(
        kernel::resource::Reservation&& charge,
        object::ObjectRef&& target,
        GrantCeiling ceiling,
        Node* parent) noexcept -> libk::Expected<GrantRef, GrantError>;
    [[nodiscard]] auto claim_slot() noexcept
        -> libk::Expected<Slot*, GrantError>;
    [[nodiscard]] auto make_page() noexcept
        -> libk::Expected<PageHeader*, GrantError>;
    [[nodiscard]] auto find(GrantKey key) noexcept -> Node*;
    [[nodiscard]] auto find(GrantKey key) const noexcept -> const Node*;
    [[nodiscard]] static auto key_of(const Node& node) noexcept -> GrantKey;
    [[nodiscard]] auto try_ref(Node& node) noexcept
        -> libk::Expected<GrantRef, GrantError>;
    [[nodiscard]] auto try_acquire(Node& node) noexcept
        -> libk::Expected<GrantLease, GrantError>;
    void drop_ref(GrantKey key) noexcept;
    void drop_lease(void* node, u64 generation) noexcept;
    [[nodiscard]] auto detach(GrantAttachment& attachment) noexcept -> bool;
    void reclaim(GrantKey key, bool drop_reference) noexcept;
    void commit_allocation(kernel::resource::Allocation& allocation) noexcept;
    void abort_allocation(kernel::resource::Allocation& allocation) noexcept;
    void revoke_allocation(kernel::resource::Allocation& allocation) noexcept;
    void stop_allocation(kernel::resource::Allocation& allocation) noexcept;
    void retire_allocation(kernel::resource::Allocation& allocation) noexcept;
    void release_page(PageHeader& page) noexcept;
    [[nodiscard]] auto revoke(
        GrantKey source,
        GrantRevoke& completion,
        bool include_source) noexcept -> libk::Expected<void, GrantError>;
    [[nodiscard]] auto create_revoke_wait() noexcept
        -> libk::Expected<GrantRevokeWait*, GrantError>;
    void destroy_revoke_wait(GrantRevokeWait& operation) noexcept;
    [[nodiscard]] auto create_close_wait() noexcept
        -> libk::Expected<kernel::resource::CloseWait*, GrantError>;
    void destroy_close_wait(kernel::resource::CloseWait& operation) noexcept;
    [[nodiscard]] static auto descendant_of(
        const Node& node,
        const Node& root) noexcept -> bool;

    kernel::mm::Pmm* pmm_{};
    Quota quota_{};
    mutable libk::TicketSpinLock lock_{};
    kernel::mm::NodePool<GrantRevokeWait> revoke_waits_;
    kernel::mm::NodePool<kernel::resource::CloseWait> close_waits_;
    PageHeader* pages_{};
    usize page_count_{};
    usize live_nodes_{};
    usize quarantined_slots_{};
};

static_assert(GrantGraph::Quota{}.nodes != 0);

} // namespace kernel::cap
