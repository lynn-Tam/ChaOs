#pragma once

#include <cap/grant.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/delegate.hpp>
#include <libk/noncopyable.hpp>
#include <sync/lock.hpp>
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
    using WorkNotifier = libk::delegate<void() noexcept>;

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
    [[nodiscard]] auto derive_tunnel_tx(
        kernel::resource::Reservation&& charge,
        const GrantLease& source,
        object::ObjectRef&& target,
        GrantCeiling ceiling,
        TunnelConnectProof proof) noexcept
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
    [[nodiscard]] auto service(usize budget) noexcept -> bool;
    [[nodiscard]] auto work_pending() const noexcept -> bool;
    void bind_work_notifier(WorkNotifier notifier) noexcept;
    void unbind_work_notifier() noexcept;
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
        kernel::resource::Sponsorship sponsorship{};
        kernel::resource::Allocation allocation{};
    };

    struct PageHeader;
    struct Slot final {
        PageHeader* page{};
        Slot* next_free{};
        libk::IntrusiveListHook work_hook{};
        libk::Atomic<u64> generation{};
        libk::Atomic<usize> operations{};
        libk::Atomic<GrantState> state{GrantState::Revoked};
        libk::Atomic<bool> work_retained{};
        libk::Atomic<bool> occupied{};
        bool quarantined{};
        alignas(Node) byte storage[sizeof(Node)]{};

        [[nodiscard]] auto node() noexcept -> Node* {
            return reinterpret_cast<Node*>(storage);
        }
        [[nodiscard]] auto node() const noexcept -> const Node* {
            return reinterpret_cast<const Node*>(storage);
        }
    };

    static constexpr usize operation_closed =
        usize{1} << (sizeof(usize) * 8 - 1);
    [[nodiscard]] static constexpr auto operation_count(usize value) noexcept
        -> usize {
        return value & ~operation_closed;
    }
    [[nodiscard]] static constexpr auto admission_closed(usize value) noexcept
        -> bool {
        return (value & operation_closed) != 0;
    }

    using WorkQueue = libk::IntrusiveList<Slot, &Slot::work_hook>;

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
    [[nodiscard]] auto locate(GrantKey key) noexcept -> Node*;
    [[nodiscard]] auto locate(GrantKey key) const noexcept -> const Node*;
    [[nodiscard]] auto find(GrantKey key) noexcept -> Node*;
    [[nodiscard]] auto find(GrantKey key) const noexcept -> const Node*;
    [[nodiscard]] static auto key_of(const Node& node) noexcept -> GrantKey;
    [[nodiscard]] auto try_ref(Node& node) noexcept
        -> libk::Expected<GrantRef, GrantError>;
    [[nodiscard]] auto try_acquire(Slot& slot, u64 generation) noexcept
        -> libk::Expected<GrantLease, GrantError>;
    void drop_ref(void* node, u64 generation) noexcept;
    void drop_lease(void* node, u64 generation) noexcept;
    void release_operation(Slot& slot) noexcept;
    void enqueue(Slot& slot) noexcept;
    void kick_work() noexcept;
    [[nodiscard]] auto take_work() noexcept -> Slot*;
    void service_slot(Slot& slot) noexcept;
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
    mutable kernel::sync::SpinLock<kernel::sync::LockClass::GrantGraph> lock_{};
    mutable kernel::sync::SpinLock<kernel::sync::LockClass::GrantWork>
        work_lock_{};
    WorkQueue work_{};
    WorkNotifier work_notifier_{};
    kernel::mm::NodePool<GrantRevokeWait> revoke_waits_;
    kernel::mm::NodePool<kernel::resource::CloseWait> close_waits_;
    PageHeader* pages_{};
    usize page_count_{};
    usize live_nodes_{};
    usize quarantined_slots_{};
};

static_assert(GrantGraph::Quota{}.nodes != 0);

} // namespace kernel::cap
