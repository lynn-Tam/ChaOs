#pragma once

#include <expected>
#include <utility>


#include <cap/grant.hpp>
#include <base/slab.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/delegate.hpp>
#include <libk/noncopyable.hpp>
#include <sync.hpp>
#include <mm/pmm.hpp>
#include <resource/sponsorship.hpp>

namespace object {
class allocation;
}

namespace cap {

class CSpace;

struct GrantServiceResult final {
    usize processed{};
    usize progressed{};
    bool more{};
};

class GrantGraph final : private libk::noncopyable_nonmovable {
public:
    using WorkNotifier = libk::delegate<
        void() noexcept>;

    struct Quota final {
        usize nodes{4096};
    };

    explicit GrantGraph(mm::Pmm& pmm) noexcept;
    GrantGraph(mm::Pmm& pmm, Quota quota) noexcept;
    ~GrantGraph() noexcept;

    [[nodiscard]] auto create_root(
        object::ref<>&& target,
        View ceiling) noexcept -> std::expected<GrantRef, GrantError>;
    [[nodiscard]] auto create_root(
        resource::Reservation&& charge,
        object::ref<>&& target,
        View ceiling) noexcept -> std::expected<GrantRef, GrantError>;

    [[nodiscard]] auto derive(
        const GrantLease& source,
        object::ref<>&& target,
        View ceiling) noexcept -> std::expected<GrantRef, GrantError>;
    [[nodiscard]] auto derive(
        resource::Reservation&& charge,
        const GrantLease& source,
        object::ref<>&& target,
        View ceiling) noexcept -> std::expected<GrantRef, GrantError>;

    [[nodiscard]] auto ref(GrantKey key) noexcept
        -> std::expected<GrantRef, GrantError>;
    [[nodiscard]] auto acquire(GrantKey key) noexcept
        -> std::expected<GrantLease, GrantError>;
    [[nodiscard]] auto attach(
        const GrantLease& source,
        GrantAttachment& attachment) noexcept
        -> std::expected<void, GrantError>;

    [[nodiscard]] auto revoke_descendants(
        GrantKey source,
        GrantRevoke& completion) noexcept -> std::expected<void, GrantError>;
    [[nodiscard]] auto invalidate(
        GrantKey source,
        GrantRevoke& completion) noexcept -> std::expected<void, GrantError>;

    [[nodiscard]] auto state(GrantKey key) const noexcept
        -> std::expected<GrantState, GrantError>;
    [[nodiscard]] auto live_count() const noexcept -> usize;
    [[nodiscard]] auto service(usize budget) noexcept -> GrantServiceResult;
    [[nodiscard]] auto work_pending() const noexcept -> bool;
    void bind_work_notifier(WorkNotifier notifier) noexcept;
    void unbind_work_notifier() noexcept;
    [[nodiscard]] static auto node_charge() noexcept
        -> resource::budget;

private:
    friend class CSpace;
    friend class GrantRef;
    friend class GrantLease;
    friend class GrantAttachment;
    friend class object::allocation;
    friend class object::group;

    struct Slot;
    using Storage = base::slab<Slot, mm::OwnedPage, mm::page_size>;
    using PageHeader = typename Storage::page;

    struct Node final {
        libk::IntrusiveListHook child_hook{};
        using ChildList = libk::IntrusiveList<Node, &Node::child_hook>;
        using AttachmentList = libk::IntrusiveList<
            GrantAttachment,
            &GrantAttachment::grant_hook_>;

        Node(
            Slot& owner,
            object::ref<>&& target_ref,
            View ceiling,
            Node* parent_node,
            resource::Reservation&& charge) noexcept
            : slot(&owner),
              target(std::move(target_ref)),
              ceiling(ceiling),
              parent(parent_node) {
            if (charge) {
                sponsorship.commit(std::move(charge));
            }
        }

        Slot* slot{};
        object::ref<> target{};
        View ceiling{};
        Node* parent{};
        ChildList children{};
        AttachmentList attachments{};
        GrantRevoke* revoke{};
        usize refs{};
        resource::Sponsorship sponsorship{};
        object::allocation* allocation{};
    };

    struct Slot final {
        PageHeader* page{};
        Slot* next_free{};
        libk::IntrusiveListHook work_hook{};
        libk::Atomic<u64> generation{};
        libk::Atomic<usize> operations{};
        libk::Atomic<GrantState> state{GrantState::Revoked};
        libk::Atomic<bool> work_retained{};
        libk::Atomic<bool> occupied{};
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

    static constexpr usize slots_per_page = Storage::capacity();

    [[nodiscard]] auto create(
        resource::Reservation&& charge,
        object::ref<>&& target,
        View ceiling,
        Node* parent) noexcept -> std::expected<GrantRef, GrantError>;
    [[nodiscard]] auto claim_slot() noexcept
        -> std::expected<Slot*, GrantError>;
    [[nodiscard]] auto make_page() noexcept
        -> std::expected<PageHeader*, GrantError>;
    [[nodiscard]] auto locate(GrantKey key) noexcept -> Node*;
    [[nodiscard]] auto locate(GrantKey key) const noexcept -> const Node*;
    [[nodiscard]] auto find(GrantKey key) noexcept -> Node*;
    [[nodiscard]] auto find(GrantKey key) const noexcept -> const Node*;
    [[nodiscard]] static auto key_of(const Node& node) noexcept -> GrantKey;
    [[nodiscard]] auto try_ref(Node& node) noexcept
        -> std::expected<GrantRef, GrantError>;
    [[nodiscard]] auto try_acquire(Slot& slot, u64 generation) noexcept
        -> std::expected<GrantLease, GrantError>;
    void drop_ref(void* node, u64 generation) noexcept;
    void drop_lease(void* node, u64 generation) noexcept;
    void release_operation(Slot& slot) noexcept;
    void enqueue(Slot& slot) noexcept;
    void kick_work() noexcept;
    [[nodiscard]] auto take_work() noexcept -> Slot*;
    [[nodiscard]] auto service_slot(Slot& slot) noexcept -> bool;
    [[nodiscard]] auto detach(GrantAttachment& attachment) noexcept -> bool;
    void reclaim(GrantKey key, bool drop_reference) noexcept;
    [[nodiscard]] auto destroy_target(const GrantLease& source) noexcept -> std::expected<void, GrantError>;
    void revoke_allocation(object::allocation& allocation) noexcept;
    void retry_allocations() noexcept;
    void bind_allocation(GrantKey root, object::allocation& allocation) noexcept;
    void release_allocation(GrantKey root, const object::allocation* allocation) noexcept;
    void release_page(PageHeader& page) noexcept;
    [[nodiscard]] auto revoke(
        GrantKey source,
        GrantRevoke& completion,
        bool include_source) noexcept -> std::expected<void, GrantError>;
    [[nodiscard]] static auto descendant_of(
        const Node& node,
        const Node& root) noexcept -> bool;

    mm::Pmm* pmm_{};
    Quota quota_{};
    mutable sync::Spin lock_{};
    mutable sync::Spin
        work_lock_{};
    WorkQueue work_{};
    WorkNotifier work_notifier_{};
    Storage storage_{};
    object::allocation* revoke_retry_{};
    usize growing_{};
};

static_assert(GrantGraph::Quota{}.nodes != 0);

} // namespace cap
