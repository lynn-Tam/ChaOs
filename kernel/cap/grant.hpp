#pragma once

#include <libk/unique_handle.hpp>

#include <utility>

#include <cap/cap.hpp>
#include <libk/types.hpp>
#include <libk/key.hpp>
#include <expected>
#include <libk/intrusive_list.hpp>
#include <libk/inplace_vector.hpp>
#include <libk/noncopyable.hpp>
#include <atomic>
#include <optional>
#include <object/ref.hpp>
#include <sync.hpp>

#include <work.hpp>
#include <uapi/ipc.h>

#include <base/slab.hpp>
#include <libk/delegate.hpp>
#include <mm/pmm.hpp>
#include <resource/quota.hpp>

namespace resource {
class Reservation;
}

namespace mm { class VSpace; }
namespace ipc { class Channel; }

namespace cap {

class Graph;
class CSpace;
class Attachment;
class GrantRef;
class GrantPin;
enum class GrantError : u8;

struct AttachOps final {
    void (*invalidate)(
        void* context,
        object::LinkWork&& work) noexcept;
    // The final-release ticket stays busy until the owner has secured its own
    // callback lifetime; releasing it permits relation storage reuse.
    void (*released)(void* context, object::LinkWork&& done) noexcept;
    // Persistent mappings retain authority. Protocol-side observers close
    // when the last capability in their whole branch disappears.
    bool retain{true};
    // Weak observation holds storage only while invalidation is draining.
    bool observe{};
};

// Embedded in a VMM-facing Binding. Graph indexes it
// non-owningly and never exposes the Grant node to the VMM.
class Attachment final : private libk::noncopyable_nonmovable {
public:
    Attachment(
        void* context,
        const AttachOps& ops) noexcept
        : ops_(&ops), link_(context, ops.invalidate, ops.released) {}
    ~Attachment() noexcept;

    [[nodiscard]] auto attached() const noexcept -> bool;
    [[nodiscard]] auto busy() const noexcept -> bool;
    // For an attached relation, true transfers quiescence to the caller;
    // false leaves it with released(). Already detached returns quiescence.
    // Detach and the final work release choose exactly one notification owner.
    [[nodiscard]] auto detach() noexcept -> bool;
    // A detached attachment is a reusable relation cell. The graph edge must
    // already be gone and all invalidation work drained before resetting it.
    void reset() noexcept;
    auto acquire() const noexcept -> std::expected<GrantPin, GrantError>;
    auto id() const noexcept -> object::identity<Graph>;
    auto record(u32 kind) const noexcept -> LinkRecord;

private:
    friend class Graph;
    friend class CSpace;
    template<usize, bool, bool> friend class Bindings;

    libk::intrusive_list_hook grant_hook_{};
    Graph* graph_{};
    void* node_{};
    u64 generation_{};
    const AttachOps* ops_{};
    object::Link link_;

};

using GrantKey = libk::key<Graph>;
using GrantId = object::identity<Graph>;

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

// An acquired invocation may finish while revoke drains its operations.
// Publishing new capabilities or persistent relations still needs Admission.
class GrantPin final : private libk::noncopyable {
public:
    GrantPin() noexcept = default;
    GrantPin(GrantPin&&) noexcept = default;
    auto operator=(GrantPin&&) noexcept -> GrantPin& = default;
    ~GrantPin() noexcept = default;

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(h_);
    }
    [[nodiscard]] auto key() const noexcept -> GrantKey;
    [[nodiscard]] auto id() const noexcept -> GrantId;
    [[nodiscard]] auto graph() const noexcept -> Graph&;
    [[nodiscard]] auto kind() const noexcept -> ObjKind;
    [[nodiscard]] auto get() const noexcept -> void*;
    [[nodiscard]] auto target_live() const noexcept -> bool;
    [[nodiscard]] auto target_id() const noexcept -> object::Id;
    [[nodiscard]] auto ceiling() const noexcept -> View;
    [[nodiscard]] auto clone() const noexcept -> std::expected<GrantRef, GrantError>;
    auto target() const noexcept -> const object::ref<>&;
    [[nodiscard]] auto clone_target() const noexcept
        -> std::expected<object::ref<>, object::error>;
    [[nodiscard]] auto attach(Attachment& attachment) const noexcept
        -> std::expected<void, GrantError>;
    void reset() noexcept { use_.reset(); h_.reset(); }

private:
    friend class Graph;
    friend class mm::VSpace;
    friend class ipc::Channel;
    // Only the object-owned transaction may change its grant's meaning.
    [[nodiscard]] auto mint(resource::Reservation&&, View) const noexcept
        -> std::expected<GrantRef, GrantError>;
    GrantPin(
        Graph& graph,
        void* node,
        u64 generation, object::pin&& use) noexcept
        : h_(Data{&graph, node, generation}), use_(std::move(use)) {}

    struct Data {
        Graph* graph{};
        void* node{};
        u64 gen{};
        static auto empty() noexcept -> Data { return {}; }
        static bool is_empty(const Data& d) noexcept { return !d.graph; }
    };
    struct Drop { void operator()(Data& d) const noexcept; };
    libk::unique_handle<Data, Drop, Data> h_{};
    object::pin use_;
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
    [[nodiscard]] auto id() const noexcept -> GrantId;
    [[nodiscard]] auto target_id() const noexcept -> object::Id;
    [[nodiscard]] auto graph() const noexcept -> Graph&;
    [[nodiscard]] auto clone() const noexcept
        -> std::expected<GrantRef, GrantError>;
    [[nodiscard]] auto acquire() const noexcept
        -> std::expected<GrantPin, GrantError>;
    void reset() noexcept { h_.reset(); }

private:
    friend class Graph;
    GrantRef(
        Graph& graph,
        void* slot,
        u64 generation, GrantId id, object::Id target) noexcept
        : h_(Data{&graph, slot, generation, id, target}) {}

    struct Data {
        Graph* graph{};
        void* slot{};
        u64 gen{};
        GrantId id{};
        object::Id target{};
        static auto empty() noexcept -> Data { return {}; }
        static bool is_empty(const Data& d) noexcept { return !d.graph; }
    };
    struct Drop { void operator()(Data& d) const noexcept; };
    libk::unique_handle<Data, Drop, Data> h_{};
};

// The owning value moves unchanged between slots and queue escrow. A private
// linear facet carries provider cleanup without adding a second authority map.
struct Capability final {
    struct Facet {
        object::Key target{};
        u64 token{};
        void* store{};
        static auto empty() noexcept -> Facet { return {}; }
        static auto is_empty(const Facet& f) noexcept -> bool { return !f.token; }
        struct Drop {
            void (*drop)(Facet&) noexcept{};
            void operator()(Facet& f) const noexcept { if (drop) drop(f); }
        };
    };
    GrantRef grant{};
    View view{};
    libk::unique_handle<Facet, Facet::Drop, Facet> facet{};
    // Keep the type/generation after consumption; never restore ordinary Send.
    void disarm() noexcept { facet.get_deleter().drop = nullptr; }
};

template<typename T>
class Resolved final : private libk::noncopyable {
public:
    Resolved(Resolved&&) noexcept = default;
    auto operator=(Resolved&&) noexcept -> Resolved& = default;

    [[nodiscard]] auto object() noexcept -> std::add_lvalue_reference_t<T> requires (!std::is_void_v<T>) { return *static_cast<T*>(pin_.get()); }
    [[nodiscard]] auto object() const noexcept -> std::add_lvalue_reference_t<const T> requires (!std::is_void_v<T>) { return *static_cast<T*>(pin_.get()); }
    [[nodiscard]] auto operator->() noexcept -> T* { return static_cast<T*>(pin_.get()); }
    [[nodiscard]] auto operator->() const noexcept -> const T* {
        return static_cast<T*>(pin_.get());
    }
    [[nodiscard]] auto rights() const noexcept -> Rights {
        return view_.rights;
    }
    [[nodiscard]] auto view() const noexcept -> View {
        return view_;
    }
    [[nodiscard]] auto grant() const noexcept -> GrantKey {
        return pin_.key();
    }
    [[nodiscard]] auto reference() const noexcept
        -> std::expected<object::ref<>, object::error> {
        return pin_.clone_target();
    }
    [[nodiscard]] auto attach(Attachment& attachment) const noexcept
        -> std::expected<void, GrantError> {
        return pin_.attach(attachment);
    }
    [[nodiscard]] auto pin() const noexcept -> const GrantPin& { return pin_; }
    auto token() const noexcept -> u64 { return token_; }
    auto selector() const noexcept -> Handle { return selector_; }
    void reset() noexcept { pin_.reset(); }
    [[nodiscard]] auto source() const noexcept -> CSpace& { return *source_; }

private:
    friend class CSpace;
    Resolved(
        CSpace& source,
        GrantPin&& pin,
        View view, Handle selector, u64 token) noexcept
        : source_(&source), selector_(selector), token_(token),
          pin_(std::move(pin)),
          view_(view) {}

    CSpace* source_{};
    Handle selector_;
    u64 token_{};
    GrantPin pin_;
    View view_{};
};

class Graph final : private libk::noncopyable_nonmovable {
public:
    // A short metadata read owns observers only, including after payload drain.
    class Sample final : private libk::noncopyable {
        friend class Graph;
        Source* source_{};
        Source::Pin pin_;
        Sample(Source& source, Source::Pin pin) noexcept : source_(&source), pin_(pin) {}
    public:
        Sample(Sample&& other) noexcept : source_(std::exchange(other.source_, nullptr)), pin_(std::exchange(other.pin_, {})) {}
        ~Sample() noexcept { reset(); }
        void reset() noexcept { auto pin = std::exchange(pin_, {}); source_ = nullptr; if (pin) pin(false); }
        auto events() const noexcept -> Source& { libk_assert(source_); return *source_; }
    };
    struct Attach { const GrantPin* grant{}; Attachment* attachment{}; const object::ref<>* target{}; };
    static constexpr usize max_attachments = IPC_MAX_CAPS;
    class Admission {
        friend class Graph;
        friend class CSpace;
        Graph* graph_{};
        std::span<const GrantPin*> grants_;
        Admission(Graph& graph, std::span<const GrantPin*> grants) noexcept : graph_(&graph), grants_(grants) {}
    public:
        Admission() noexcept = default; // Trusted internal calls carry no Grant authority.
        auto attach(std::span<const Attach>, std::span<const object::ref<>*> gates,
            libk::delegate<void() noexcept> commit) const noexcept -> std::expected<void, GrantError>;
        auto attach(const GrantPin& grant, Attachment& attachment, std::span<const object::ref<>*> gates,
            libk::delegate<void() noexcept> commit) const noexcept -> std::expected<void, GrantError> {
            const Attach item{&grant, &attachment}; return attach(std::span{&item, 1}, gates, commit);
        }
        template<class Commit> auto publish(Commit&& commit) const noexcept -> bool {
            if (!graph_) return commit();
            auto invoke = [&]() noexcept { return commit(); };
            return graph_->publish(grants_, libk::delegate<bool() noexcept>::bind(invoke));
        }
        template<class Commit> auto publish(std::span<const object::ref<>*> gates, Commit&& commit) const noexcept -> status_t {
            status_t status = STATUS_BUSY;
            (void)publish([&] {
                status = object::ref<>::publish(gates, commit) ? STATUS_OK : STATUS_CLOSED;
                return true;
            });
            return status;
        }
    };
    auto admission(std::span<const GrantPin*> grants) noexcept -> Admission { return {*this, grants}; }
    // Ancestor checks are bounded; installed attachments do not walk this path.
    static constexpr u16 max_depth = 64;
    struct QuotaLimit final {
        usize nodes{4096};
    };

    explicit Graph(mm::Pmm& pmm, ::WorkQueue& work) noexcept;
    Graph(mm::Pmm& pmm, ::WorkQueue& work, QuotaLimit quota) noexcept;
    ~Graph() noexcept;

    [[nodiscard]] auto create_root(
        object::ref<> target,
        View ceiling) noexcept -> std::expected<GrantRef, GrantError>;
    [[nodiscard]] auto create_root(
        resource::Reservation&& usage,
        object::ref<> target,
        View ceiling) noexcept -> std::expected<GrantRef, GrantError>;

    [[nodiscard]] auto derive(
        const GrantPin& source,
        object::ref<> target,
        View ceiling) noexcept -> std::expected<GrantRef, GrantError>;
    [[nodiscard]] auto derive(
        resource::Reservation&& usage,
        const GrantPin& source,
        object::ref<> target,
        View ceiling) noexcept -> std::expected<GrantRef, GrantError>;

    [[nodiscard]] auto ref(GrantKey key) noexcept
        -> std::expected<GrantRef, GrantError>;
    [[nodiscard]] auto acquire(GrantKey key) noexcept
        -> std::expected<GrantPin, GrantError>;
    [[nodiscard]] auto attach(
        const GrantPin& source,
        Attachment& attachment) noexcept
        -> std::expected<void, GrantError>;

    // Monotonic full-branch close. A missing generation is already drained.
    auto close(GrantKey, Source::Context* = nullptr, GrantId selected = {}) noexcept -> WaitResult;
    auto status(GrantKey) const noexcept -> WaitResult;
    auto observe(GrantKey seed, GrantId selected, usize depth) noexcept -> std::expected<Sample, GrantError>;

    // A metadata projection: no business pin or enduring observer reference.
    auto view(GrantKey, View) const noexcept -> std::optional<CapView>;
    auto inspect(GrantKey seed, GrantQuery, usize depth, GrantRecords&) const noexcept -> status_t;
    [[nodiscard]] auto state(GrantKey key) const noexcept
        -> std::expected<GrantState, GrantError>;
    [[nodiscard]] auto live_count() const noexcept -> usize;
    [[nodiscard]] auto work_pending() const noexcept -> bool;
    [[nodiscard]] static auto node_usage() noexcept
        -> resource::Budget;

private:
    friend class CSpace;
    friend class GrantRef;
    friend class GrantPin;
    friend class Attachment;

    auto publish(std::span<const GrantPin*>, libk::delegate<bool() noexcept>) noexcept -> bool;
    auto attach_commit(std::span<const Attach>, std::span<object::Link::Prepared>,
        std::span<const object::ref<>*>, libk::delegate<void() noexcept>,
        std::span<const GrantPin*> = {}) noexcept -> std::expected<void, GrantError>;
    struct Slot;
    using Storage = base::slab<Slot, mm::OwnedPage, mm::page_size>;
    using PageHeader = typename Storage::page;

    struct Node final {
        libk::intrusive_tree_hook child_hook{}, id_hook{};
        auto id() const noexcept -> u64;
        struct Compare {
            bool operator()(const Node& a, const Node& b) const noexcept { return a.id() < b.id(); }
            bool operator()(u64 a, const Node& b) const noexcept { return a < b.id(); }
            bool operator()(const Node& a, u64 b) const noexcept { return a.id() < b; }
        };
        using Children = libk::intrusive_tree<Node, &Node::child_hook, Compare>;
        using AttachmentList = libk::intrusive_list<
            Attachment,
            &Attachment::grant_hook_>;

        Node(
            Graph& graph,
            Slot& owner,
            object::ref<> target_ref,
            View ceiling,
            Node* parent_node,
            resource::Reservation&& usage) noexcept
            : graph(&graph), slot(&owner),
              kind(target_ref.kind()), target(std::move(target_ref)),
              ceiling(ceiling),
              parent(parent_node), root(parent_node ? parent_node->root : this),
              depth(parent_node ? parent_node->depth + 1 : 0) {
            events.bind_work(graph.executor_, {}, {}, Source::Pin::bind<&Node::publisher>(*this));
            events.update(0);
            if (usage) {
                allocation.commit(std::move(usage));
            }
        }

        Graph* graph{};
        Slot* slot{};
        ObjKind kind{};
        object::ref<> target{};
        View ceiling{};
        Node* parent{};
        Node* const root;
        std::atomic<u64> epoch{1}, validated{};
        u16 depth{};
        Children children{};
        u64 version{1};
        AttachmentList attachments{};
        Attachment* attach_scan{};
        usize retaining{};
        Node* scan{};
        usize pending{}, observers{};
        bool acknowledged{}, dirty{};
        Src<STATE_DRAINED> events;
        void publisher(bool) noexcept;
        void release_wait() noexcept;
        usize refs{};
        resource::Allocation allocation{};
        object::Link lifetime{this, &Graph::invalidate_object, &Graph::release_object};
        object::LinkWork object_work;
    };

    struct Slot final {
        mutable sync::Spin lock;
        PageHeader* page{};
        Slot* next_free{};
        libk::intrusive_list_hook work_hook{};
        GrantId id{};
        std::atomic<u64> generation{};
        std::atomic<usize> operations{operation_closed};
        std::atomic<GrantState> state{GrantState::Revoked};
        std::atomic<bool> work_retained{};
        std::atomic<bool> occupied{};
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

    using SlotQueue = libk::intrusive_list<Slot, &Slot::work_hook>;

    static void invalidate_object(void*, object::LinkWork&&) noexcept;
    static void release_object(void*, object::LinkWork&&) noexcept;

    static constexpr usize slots_per_page = Storage::capacity();

    [[nodiscard]] auto create(
        resource::Reservation&& usage,
        object::ref<> target,
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
    [[nodiscard]] auto valid(Node& node) noexcept -> bool;
    [[nodiscard]] auto try_ref(Slot& slot, u64 generation) noexcept
        -> std::expected<GrantRef, GrantError>;
    [[nodiscard]] auto try_acquire(Slot& slot, u64 generation) noexcept
        -> std::expected<GrantPin, GrantError>;
    auto acquire_locked(Slot&, u64 generation) noexcept -> std::expected<GrantPin, GrantError>;
    void drop_ref(void* node, u64 generation) noexcept;
    void drop_pin(void* node, u64 generation) noexcept;
    void release_operation(Slot& slot) noexcept;
    void enqueue(Slot& slot) noexcept;
    void kick_work() noexcept;
    void run_work() noexcept;
    auto service(usize budget) noexcept -> bool;
    [[nodiscard]] auto take_work() noexcept -> Slot*;
    void service_slot(Slot& slot) noexcept;
    [[nodiscard]] auto detach(Attachment& attachment) noexcept -> bool;
    void reclaim(GrantKey key, bool drop_reference) noexcept;
    void changed(Node&) noexcept;
    void close_node(Node&) noexcept;
    void acknowledge(Node&) noexcept;
    void release_page(PageHeader& page) noexcept;

    mm::Pmm* pmm_{};
    QuotaLimit quota_{};
    mutable sync::Spin lock_{};
    mutable sync::Spin
        work_lock_{};
    SlotQueue work_{};
    ::WorkQueue& executor_;
    Work job_;
    libk::intrusive_tree<Node, &Node::id_hook, Node::Compare> ids_;
    Storage storage_{};
    usize growing_{};
};

static_assert(Graph::QuotaLimit{}.nodes != 0);

// Fixed admission members share one publisher/drain boundary. Domain state
// remains in the owner; this only owns its actual Grant relations and callbacks.
template<usize N, bool Retain = true, bool Observe = false>
class Bindings final : private libk::noncopyable_nonmovable {
    static_assert(N && N <= Graph::max_attachments);
public:
    using Action = libk::delegate<void() noexcept>;
    class Hold final : private libk::noncopyable {
        Bindings* owner_{};
        object::ref<> storage_;
        friend class Bindings;
        explicit Hold(Bindings& owner) noexcept
            : owner_(&owner), storage_(object::ref<>::retain_alive(owner.key_)) {
            sync::Lock guard{owner.lock_}; ++owner.calls_;
        }
    public:
        Hold(Hold&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)), storage_(std::move(other.storage_)) {}
        ~Hold() noexcept { if (owner_) owner_->release(); }
    };
private:
    struct Cell {
        explicit Cell(Bindings& owner) noexcept : owner(owner), attachment(this, ops_) {}
        Bindings& owner;
        Attachment attachment;
        object::LinkWork work;
        bool used{}, detaching{}, released{true};
    };
    inline static const AttachOps ops_{
        .invalidate = [](void* p, object::LinkWork&& work) noexcept {
            auto& cell = *static_cast<Cell*>(p); cell.owner.invalidate(cell, std::move(work));
        },
        .released = [](void* p, object::LinkWork&& done) noexcept {
            auto& cell = *static_cast<Cell*>(p);
            auto publishing = cell.owner.hold();
            done.reset();
            sync::Lock guard{cell.owner.lock_}; cell.released = true;
        },
        .retain = Retain, .observe = Observe,
    };
    mutable sync::Spin lock_;
    std::array<Cell, N> cells_;
    object::Key key_{};
    Action invalidate_, drained_;
    usize calls_{};
    u64 epoch_{1};
    void changed() noexcept { if (epoch_ != std::numeric_limits<u64>::max()) ++epoch_; }
    enum class Phase : u8 { Idle, Live, Closing, Closed } phase_{};
    bool revoked_{};
    template<usize... I>
    Bindings(Action invalidate, Action drained, std::index_sequence<I...>) noexcept
        : cells_{((void)I, Cell{*this})...}, invalidate_(invalidate), drained_(drained) {}
    auto released() const noexcept -> bool {
        libk_assert(lock_.held());
        return std::ranges::all_of(cells_, [](const auto& cell) {
            return cell.released && !cell.work && !cell.attachment.attached() && !cell.attachment.busy();
        });
    }
    void release() noexcept {
        Action drained;
        { sync::Lock guard{lock_};
          libk_assert(calls_);
          if (--calls_ == 0 && (phase_ == Phase::Closing || phase_ == Phase::Closed) && released()) {
              if (phase_ != Phase::Closed) changed();
              phase_ = Phase::Closed; drained = drained_;
          }
        }
        if (drained) drained(); // Last owner access; Hold still owns storage.
    }
    void invalidate(Cell& cell, object::LinkWork&& work) noexcept {
        auto publishing = hold();
        Action invalidate;
        bool closing;
        { sync::Lock guard{lock_};
          libk_assert(!cell.work); cell.work = std::move(work);
          if (!std::exchange(revoked_, true)) invalidate = invalidate_;
          closing = phase_ == Phase::Closing || phase_ == Phase::Closed;
        }
        if (invalidate) invalidate();
        if (closing) static_cast<void>(close()); // Also drain late invalidations.
    }
public:
    Bindings(Action invalidate, Action drained) noexcept
        : Bindings(invalidate, drained, std::make_index_sequence<N>{}) {}
    ~Bindings() noexcept { sync::Lock guard{lock_}; libk_assert(!calls_ && released()); }
    auto key() const noexcept -> object::Key { return key_; }
    void bind(object::Key key) noexcept { libk_assert(!key_.valid()); key_ = key; }
    auto hold() noexcept -> Hold { return Hold{*this}; }
    auto revoked() const noexcept -> bool { sync::Lock guard{lock_}; return revoked_; }
    auto drained() const noexcept -> bool {
        sync::Lock guard{lock_}; return (phase_ == Phase::Idle || phase_ == Phase::Closed) && !calls_;
    }
    auto ready() const noexcept -> bool {
        sync::Lock guard{lock_};
        return phase_ == Phase::Idle || phase_ == Phase::Live || (phase_ == Phase::Closed && !calls_)
            || std::ranges::any_of(cells_, [](const auto& cell) { return bool(cell.work); });
    }
    auto reset() noexcept -> bool {
        sync::Lock guard{lock_};
        if (phase_ == Phase::Idle) return true;
        if (phase_ != Phase::Closed || calls_) return false;
        for (auto& cell : cells_) {
            cell.attachment.reset(); cell.used = cell.detaching = false; cell.released = true;
        }
        revoked_ = false; phase_ = Phase::Idle; changed(); return true;
    }
    template<class Publish>
    auto admit(Publish&& publish, Action commit = {}) noexcept -> status_t {
        auto publishing = hold();
        sync::Lock guard{lock_};
        if (phase_ != Phase::Idle) return STATUS_BUSY;
        std::array<Attachment*, N> cells;
        for (usize i = 0; i < N; ++i) cells[i] = &cells_[i].attachment;
        auto install = [&]() noexcept {
            phase_ = Phase::Live; changed();
            for (auto& cell : cells_) { cell.used = true; cell.released = false; }
            if (commit) commit();
        };
        return std::forward<Publish>(publish)(std::span{cells}, Action::bind(install));
    }
    auto attach(std::span<const GrantPin*> grants, std::span<const object::ref<>*> gates = {},
        Action commit = {}) noexcept -> status_t {
        if (grants.size() != N || !grants.front() || !*grants.front()) return STATUS_BAD_ARGS;
        return admit([&](std::span<Attachment*> cells, Action install) noexcept {
            std::array<Graph::Attach, N> items;
            for (usize i = 0; i < N; ++i) items[i] = {grants[i], cells[i]};
            return grants.front()->graph().admission(grants).attach(items, gates, install) ? STATUS_OK : STATUS_CLOSED;
        }, commit);
    }
    auto attach(std::span<const object::ref<>*> targets, Action commit = {}) noexcept -> status_t {
        if (targets.size() != N) return STATUS_BAD_ARGS;
        return admit([&](std::span<Attachment*> cells, Action install) noexcept {
            std::array<Graph::Attach, N> items;
            for (usize i = 0; i < N; ++i) items[i] = {nullptr, cells[i], targets[i]};
            return Graph::Admission{}.attach(items, {}, install) ? STATUS_OK : STATUS_CLOSED;
        }, commit);
    }
    void links(LinkRecords& out, u32 kind, u64 cursor) const noexcept {
        sync::Lock guard{lock_};
        out.epoch = epoch_ > std::numeric_limits<u64>::max() - out.epoch
            ? std::numeric_limits<u64>::max() : out.epoch + epoch_;
        if (phase_ != Phase::Live) return;
        for (const auto& cell : cells_) {
            auto record = cell.attachment.record(kind);
            if (!record.link_id || record.link_id <= cursor) continue;
            libk_assert(out.count < LINK_RECORD_PAGE);
            auto* begin = out.records; auto* end = begin + out.count++;
            auto* position = std::lower_bound(begin, end, record.link_id,
                [](const LinkRecord& row, u64 id) { return row.link_id < id; });
            std::move_backward(position, end, end + 1); *position = record;
        }
    }
    // One publication boundary for already accepted authority. Holding this
    // lock excludes detach/reuse; Graph and ordered Gates decide against close.
    auto publish(Action commit) noexcept -> bool requires (!Observe) {
        auto publishing = hold();
        std::array<object::ref<>, N> targets;
        std::array<GrantPin, N> pins;
        std::array<const object::ref<>*, N> gates;
        libk::inplace_vector<const GrantPin*, N> grants;
        sync::Lock guard{lock_};
        if (phase_ == Phase::Idle) { commit(); return true; }
        if (phase_ != Phase::Live || revoked_) return false;
        for (usize i = 0; i < N; ++i) {
            auto& attachment = cells_[i].attachment;
            targets[i] = attachment.link_.reference();
            if (!targets[i]) return false;
            gates[i] = &targets[i];
            if (attachment.graph_) {
                auto pin = attachment.acquire();
                if (!pin) return false;
                pins[i] = std::move(*pin);
                if (!grants.empty() && &pins[i].graph() != &grants.front()->graph()) return false;
                (void)grants.try_emplace_back(&pins[i]);
            }
        }
        if (grants.empty()) return object::ref<>::publish(gates, commit);
        return grants.front()->graph().admission(grants.span()).publish(gates, commit) == STATUS_OK;
    }
    auto close() noexcept -> bool {
        { sync::Lock guard{lock_}; if (phase_ == Phase::Closed) return !calls_; }
        {
            auto publishing = hold();
            { sync::Lock guard{lock_}; if (phase_ != Phase::Closing) changed(); phase_ = Phase::Closing; }
            // Issue all detaches before returning any invalidation work.
            for (auto& cell : cells_) {
                bool detach;
                { sync::Lock guard{lock_}; detach = cell.used && !std::exchange(cell.detaching, true); }
                if (detach && cell.attachment.detach()) {
                    sync::Lock guard{lock_}; cell.released = true;
                }
            }
            for (auto& cell : cells_) {
                object::LinkWork work;
                { sync::Lock guard{lock_}; work = std::move(cell.work); }
                work.reset();
            }
        }
        return drained();
    }
};

} // namespace cap
