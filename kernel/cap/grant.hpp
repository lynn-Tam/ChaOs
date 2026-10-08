#pragma once

#include <libk/unique_handle.hpp>

#include <utility>

#include <cap/cap.hpp>
#include <base/types.hpp>
#include <libk/key.hpp>
#include <expected>
#include <libk/intrusive_list.hpp>
#include <libk/noncopyable.hpp>
#include <libk/sync/atomic.hpp>
#include <object/id.hpp>
#include <object/ref.hpp>
#include <sync.hpp>
#include <wait.hpp>

#include <work.hpp>

#include <base/slab.hpp>
#include <libk/delegate.hpp>
#include <mm/pmm.hpp>
#include <resource/sponsorship.hpp>

namespace object {
class allocation;
}

namespace resource {
class Reservation;
}

namespace mm { class VSpace; }
namespace ipc { class Channel; }

namespace cap {

class Graph;
class CSpace;
class GrantAttachment;
class GrantRef;

class GrantWork final : private libk::noncopyable {
public:
    GrantWork() noexcept = default;
    GrantWork(GrantWork&& other) noexcept;
    auto operator=(GrantWork&& other) noexcept -> GrantWork&;
    ~GrantWork() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept {
        return attachment_ != nullptr;
    }
    void reset() noexcept;

private:
    friend class CSpace;
    friend class Graph;
    explicit GrantWork(GrantAttachment& attachment) noexcept
        : attachment_(&attachment) {}

    GrantAttachment* attachment_{};
};

struct GrantAttachmentOps final {
    void (*invalidate)(
        void* context,
        GrantWork&& work) noexcept;
    void (*released)(void* context) noexcept;
};

// Embedded in a VMM-facing Backing. Graph indexes it
// non-owningly and never exposes the Grant node to the VMM.
class GrantAttachment final : private libk::noncopyable_nonmovable {
public:
    GrantAttachment(
        void* context,
        const GrantAttachmentOps& ops) noexcept
        : context_(context), ops_(&ops) {}
    ~GrantAttachment() noexcept;

    [[nodiscard]] auto attached() const noexcept -> bool;
    [[nodiscard]] auto busy() const noexcept -> bool;
    // For an attached relation, true transfers quiescence to the caller;
    // false leaves it with released(). Already detached returns quiescence.
    // Detach and the final work release choose exactly one notification owner.
    [[nodiscard]] auto detach() noexcept -> bool;
    // A detached attachment is a reusable relation cell. The graph edge must
    // already be gone and all invalidation work drained before resetting it.
    void reset() noexcept;

private:
    friend class Graph;
    friend class GrantWork;

    enum class State : u8 {
        Idle,
        Attached,
        Invalidating,
        Draining,
        Detached,
    };

    void drop_work() noexcept;

    libk::IntrusiveListHook grant_hook_{};
    Graph* graph_{};
    void* node_{};
    u64 generation_{};
    void* context_{};
    const GrantAttachmentOps* ops_{};
    libk::Atomic<usize> work_{};
    libk::Atomic<u8> state_{static_cast<u8>(State::Idle)};
};

using GrantKey = libk::key<Graph>;

enum class GrantState : u8 {
    Live,
    Revoking,
    Revoked,
};

enum class GrantError : u8 {
    InvalidKey,
    InvalidState,
    WrongKind,
    RightsViolation,
    OutOfMemory,
    QuotaExceeded,
    GenerationExhausted,
    RevocationConflict,
};

class GrantLease final : private libk::noncopyable {
public:
    GrantLease() noexcept = default;
    GrantLease(GrantLease&&) noexcept = default;
    auto operator=(GrantLease&&) noexcept -> GrantLease& = default;
    ~GrantLease() noexcept = default;

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(h_);
    }
    [[nodiscard]] auto key() const noexcept -> GrantKey;
    [[nodiscard]] auto graph() const noexcept -> Graph&;
    [[nodiscard]] auto kind() const noexcept -> object::ObjectKind;
    [[nodiscard]] auto get() const noexcept -> void*;
    [[nodiscard]] auto target_live() const noexcept -> bool;
    [[nodiscard]] auto ceiling() const noexcept -> View;
    [[nodiscard]] auto clone_target() const noexcept
        -> std::expected<object::ref<>, object::error>;
    [[nodiscard]] auto attach(GrantAttachment& attachment) const noexcept
        -> std::expected<void, GrantError>;
    void reset() noexcept { h_.reset(); }

private:
    friend class Graph;
    friend class mm::VSpace;
    friend class ipc::Channel;
    // Only the object-owned transaction may change its grant's meaning.
    [[nodiscard]] auto mint(resource::Reservation&&, View) const noexcept
        -> std::expected<GrantRef, GrantError>;
    GrantLease(
        Graph& graph,
        void* node,
        u64 generation) noexcept
        : h_(Data{&graph, node, generation}) {}

    struct Data {
        Graph* graph{};
        void* node{};
        u64 gen{};
        static auto empty() noexcept -> Data { return {}; }
        static bool is_empty(const Data& d) noexcept { return !d.graph; }
    };
    struct Drop { void operator()(Data& d) const noexcept; };
    libk::unique_handle<Data, Drop, Data> h_{};
};

class GrantRef final : private libk::noncopyable {
public:
    GrantRef() noexcept = default;
    GrantRef(GrantRef&&) noexcept = default;
    auto operator=(GrantRef&&) noexcept -> GrantRef& = default;
    ~GrantRef() noexcept = default;

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(h_);
    }
    [[nodiscard]] auto key() const noexcept -> GrantKey;
    [[nodiscard]] auto graph() const noexcept -> Graph&;
    [[nodiscard]] auto clone() const noexcept
        -> std::expected<GrantRef, GrantError>;
    [[nodiscard]] auto acquire() const noexcept
        -> std::expected<GrantLease, GrantError>;
    void reset() noexcept { h_.reset(); }

private:
    friend class Graph;
    GrantRef(
        Graph& graph,
        void* slot,
        u64 generation) noexcept
        : h_(Data{&graph, slot, generation}) {}

    struct Data {
        Graph* graph{};
        void* slot{};
        u64 gen{};
        static auto empty() noexcept -> Data { return {}; }
        static bool is_empty(const Data& d) noexcept { return !d.graph; }
    };
    struct Drop { void operator()(Data& d) const noexcept; };
    libk::unique_handle<Data, Drop, Data> h_{};
};

// Caller-owned completion for one lineage transition. Existing operation
// leases may finish, but the ticket cannot disappear while nodes reference it.
class GrantRevoke final : private libk::noncopyable_nonmovable {
public:
    explicit GrantRevoke(
        sync::Latch::Notifier notifier = {}) noexcept
        : completion_(notifier) {}
    ~GrantRevoke() noexcept = default;

    [[nodiscard]] auto initialized() const noexcept -> bool {
        return completion_.initialized();
    }
    [[nodiscard]] auto complete() const noexcept -> bool {
        return completion_.complete();
    }
    [[nodiscard]] auto arm() noexcept -> bool { return completion_.arm(); }

private:
    friend class Graph;
    void initialize(usize pending) noexcept;
    void acknowledge() noexcept;
    sync::Latch completion_;
};

template<typename T>
class Resolved final : private libk::noncopyable {
public:
    Resolved(Resolved&&) noexcept = default;
    auto operator=(Resolved&&) noexcept -> Resolved& = default;

    [[nodiscard]] auto object() noexcept -> T& { return *static_cast<T*>(lease_.get()); }
    [[nodiscard]] auto object() const noexcept -> const T& { return *static_cast<T*>(lease_.get()); }
    [[nodiscard]] auto operator->() noexcept -> T* { return static_cast<T*>(lease_.get()); }
    [[nodiscard]] auto operator->() const noexcept -> const T* {
        return static_cast<T*>(lease_.get());
    }
    [[nodiscard]] auto rights() const noexcept -> Rights {
        return view_.rights;
    }
    [[nodiscard]] auto view() const noexcept -> View {
        return view_;
    }
    [[nodiscard]] auto grant() const noexcept -> GrantKey {
        return lease_.key();
    }
    [[nodiscard]] auto reference() const noexcept
        -> std::expected<object::ref<>, object::error> {
        return lease_.clone_target();
    }
    [[nodiscard]] auto attach(GrantAttachment& attachment) const noexcept
        -> std::expected<void, GrantError> {
        return lease_.attach(attachment);
    }
    [[nodiscard]] auto lease() const noexcept -> const GrantLease& { return lease_; }
    void reset() noexcept { lease_.reset(); }
    [[nodiscard]] auto source() const noexcept -> CSpace& { return *source_; }

private:
    friend class CSpace;
    Resolved(
        CSpace& source,
        GrantLease&& lease,
        View view) noexcept
        : source_(&source),
          lease_(std::move(lease)),
          view_(view) {}

    CSpace* source_{};
    GrantLease lease_;
    View view_{};
};

class Graph final : private libk::noncopyable_nonmovable {
public:
    struct Quota final {
        usize nodes{4096};
    };

    explicit Graph(mm::Pmm& pmm, ::WorkQueue& work) noexcept;
    Graph(mm::Pmm& pmm, ::WorkQueue& work, Quota quota) noexcept;
    ~Graph() noexcept;

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
    [[nodiscard]] auto work_pending() const noexcept -> bool;
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

    using SlotQueue = libk::IntrusiveList<Slot, &Slot::work_hook>;

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
    void run_work() noexcept;
    auto service(usize budget) noexcept -> bool;
    [[nodiscard]] auto take_work() noexcept -> Slot*;
    void service_slot(Slot& slot) noexcept;
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
    [[nodiscard]] static auto next(Node& root, Node& node) noexcept -> Node*;

    mm::Pmm* pmm_{};
    Quota quota_{};
    mutable sync::Spin lock_{};
    mutable sync::Spin
        work_lock_{};
    SlotQueue work_{};
    ::WorkQueue& executor_;
    Work job_;
    Storage storage_{};
    object::allocation* revoke_retry_{};
    usize growing_{};
};

static_assert(Graph::Quota{}.nodes != 0);

} // namespace cap
