#pragma once

#include <libk/assert.hpp>
#include <libk/types.hpp>
#include <expected>
#include <optional>
#include <atomic>
#include <limits>
#include <libk/noncopyable.hpp>
#include <utility>
#include <uapi/cap.h>
#include <uapi/abi.h>
#include <libk/key.hpp>
#include <type_traits>
#include <algorithm>
#include <span>
#include <libk/delegate.hpp>
#include <libk/intrusive_tree.hpp>
#include <libk/manual_lifetime.hpp>
#include <sync.hpp>

namespace ipc { class Channel; class Notification; class Port; }
namespace irq { class Irq; class Ctl; }
namespace io { class Iommu; class Space; class IoOp; }
namespace sched { class Sc; class Domain; }
namespace cap { class CSpace; }
namespace resource { class Quota; class Allocation; struct Budget; }
namespace mm { class Mem; class VSpace; class VmOp; class Phys; }
class Pager;
class Thread;

namespace object {

class Scope;
class Membership;
class anchor;
class Link;
class LinkWork;
// One table binds ABI tags to payload types and their typed storage.
template<class...> class store;
template<class T, ObjKind K> struct Def { using type = T; static constexpr ObjKind kind = K; };
template<class... E> struct Catalog {
    using objects = store<typename E::type...>;
    template<class F, class R> static auto visit(ObjKind tag, F&& fn, R missing) noexcept -> R {
        ((tag == E::kind ? (missing = fn.template operator()<typename E::type>(), true) : false) || ...);
        return missing;
    }
    template<class T> static constexpr ObjKind kind = [] {
        ObjKind k = ObjInvalid;
        ((std::is_same_v<T, typename E::type> ? k = E::kind : k), ...);
        return k;
    }();
};
using Types = Catalog<Def<mm::VmOp, ObjVmOp>, Def<io::IoOp, ObjIoOp>, Def<io::Space, ObjIoSpace>, Def<irq::Irq, ObjIrq>, Def<irq::Ctl, ObjIrqCtl>,
    Def<io::Iommu, ObjIommu>, Def<mm::VSpace, ObjVSpace>, Def<mm::Mem, ObjMem>, Def<mm::Phys, ObjPhys>,
    Def<Pager, ObjPager>, Def<cap::CSpace, ObjCSpace>,
    Def<ipc::Channel, ObjChannel>, Def<ipc::Notification, ObjNotification>, Def<ipc::Port, ObjPort>,
    Def<sched::Domain, ObjDomain>, Def<sched::Sc, ObjSc>, Def<Thread, ObjThread>, Def<resource::Quota, ObjQuota>, Def<Scope, ObjScope>>;
using Objects = Types::objects;
template<class T> inline constexpr ObjKind kind = Types::kind<T>;
// Storage keys stay private to kernel owners; public identities never encode
// an address and never repeat, even after a pool or its pages are destroyed.
using Key = libk::key<anchor>;
void changed(Key) noexcept;
template<class Tag> struct identity final {
    u64 value{};
    constexpr auto valid() const noexcept -> bool { return value != 0; }
    friend constexpr auto operator==(identity, identity) noexcept -> bool = default;
};
using Id = identity<anchor>;
inline std::atomic<u64> next_id{1};
template<class Tag> auto new_id() noexcept -> identity<Tag> {
    auto value = next_id.load(std::memory_order_relaxed);
    while (value != std::numeric_limits<u64>::max()) {
        if (next_id.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
            return {value};
    }
    return {};
}

// Preparation pins cannot publish through close. Only the final, non-failing
// index commit takes this short boundary; callbacks and teardown stay outside.
class Gate final : private libk::noncopyable_nonmovable {
    template<class> friend class ref;
    friend class Link;
    static constexpr usize closing = usize{1} << (sizeof(usize) * 8 - 1);
    std::atomic<usize> state_{};
    sync::Spin lock_{};
    Link* links_{};
    void unlink(Link&) noexcept;
public:
    auto open() const noexcept -> bool { return !(state_.load(std::memory_order_acquire) & closing); }
    auto empty() const noexcept -> bool { return (state_.load(std::memory_order_acquire) & ~closing) == 0; }
    auto enter() noexcept -> bool {
        auto state = state_.load(std::memory_order_relaxed);
        while (!(state & closing) && state != closing - 1)
            if (state_.compare_exchange_weak(state, state + 1, std::memory_order_acquire)) return true;
        return false;
    }
    auto leave() noexcept -> bool {
        auto state = state_.load(std::memory_order_relaxed);
        for (;;) {
            libk_assert((state & ~closing) != 0);
            // Keep the final closed use until the pool lock protects the zero
            // transition. Nothing may touch an anchor after publishing zero.
            if (state == closing + 1) return true;
            if (state_.compare_exchange_weak(state, state - 1,
                    std::memory_order_release, std::memory_order_relaxed)) return false;
        }
    }
    auto finish_leave() noexcept -> bool {
        const auto before = state_.fetch_sub(1, std::memory_order_acq_rel);
        libk_assert((before & closing) && (before & ~closing));
        return before == closing + 1;
    }
    template<class Commit> auto publish(Commit&& commit) noexcept -> bool {
        sync::Lock guard{lock_};
        if (!open()) return false;
        std::forward<Commit>(commit)();
        return true;
    }
    // The first closer owns a scan use before publishing Closing. An empty
    // business-use count must not bypass weak registrations or their drains.
    auto close() noexcept -> bool {
        sync::Lock guard{lock_};
        auto state = state_.load(std::memory_order_relaxed);
        for (;;) {
            if (state & closing) return false;
            libk_assert(state < closing - 1);
            if (state_.compare_exchange_weak(state, (state | closing) + 1, std::memory_order_acq_rel)) return true;
        }
    }
    // Claim callback ownership under the index lock; invoke after unlocking.
    auto invalidate(usize limit) noexcept -> bool;
    void reset() noexcept {
        libk_assert(empty() && !links_);
        state_.store(0, std::memory_order_relaxed);
    }
};

enum class error : u8 {
    out_of_memory,
    exhausted,
    invalid_id,
    closed,
    wrong_type,
    pending,
    stale,
};

// Scratch for traversing actual control ownership/quota indices. No graph
// edges or usage totals are stored here; the structure lock serializes walks.
class ControlScan;
class Control final {
    friend class ControlScan;
public:
    struct Check { u64 from{}, to{}, version{}; bool reachable{}; };
    static void changed() noexcept { libk_assert(lock.held()); libk_assert(++version_); }
    auto id() const noexcept -> u64;
    auto key() const noexcept -> Key;
    auto checked(const Check&, const Control&) const noexcept -> std::expected<bool, error>;
    inline static sync::Spin lock;
    void bind(anchor& owner) noexcept { libk_assert(!owner_); owner_ = &owner; }
    static void push(Control*& pending, Control* node, u64 epoch) noexcept {
        if (!node || node->visited_ == epoch) return;
        node->visited_ = epoch; node->next_ = pending; pending = node;
    }
    auto reaches(Control& destination) noexcept -> std::expected<bool, error> {
        libk_assert(lock.held());
        if (visits_ == std::numeric_limits<u64>::max()) return std::unexpected(error::exhausted);
        const auto visit = ++visits_;
        Control* pending{};
        push(pending, this, visit);
        for (usize step = 0; pending; ++step) {
            if (step == 64) return std::unexpected(error::pending);
            auto* node = pending; pending = node->next_;
            if (node == &destination) return true;
            node->edges(pending, visit);
        }
        return false;
    }
private:
    inline static u64 version_{1}, visits_{};
    auto parent(usize) const noexcept -> Control*;
    void edges(Control*&, u64) noexcept;
    anchor* owner_{};
    Control* next_{};
    u64 visited_{};
};

template<typename T = void>
class ref;
class cleanup;
class pin;
template<typename T>
class pool;

// Common storage header for every typed pool slot. It is deliberately
// outside the payload: kernel objects do not inherit a lifecycle base and do
// not carry capability state. The ops table returns to the actual pool so all
// Ordinary owners and admitted uses return locally; only lifecycle/index
// transitions need the pool lock.
class anchor final {
private:
    enum class phase : u8 { free, constructing, live, closing, draining, quiescent };

    struct Ops final {
        void (*retain)(void*, anchor&, u64) noexcept;
        bool (*live)(void*, anchor&, u64) noexcept;
        bool (*try_retain)(void*, anchor&, u64) noexcept;
        void (*drop_ref)(void*, anchor&, u64) noexcept;
        void* (*payload)(anchor&) noexcept;
        bool (*destroy)(void*, anchor&, u64) noexcept;
        bool (*request_retire)(void*, anchor&, u64) noexcept;
        void (*finish_cleanup)(void*, anchor&, u64) noexcept;
        void (*release_use)(void*, anchor&, u64) noexcept;
        Control* (*control)(anchor&) noexcept;
        resource::Allocation* (*allocation)(anchor&) noexcept;
    };

    template<typename T>
    friend class pool;
    template<typename T>
    friend class ref;
    friend class cleanup;
    friend class pin;
    friend class Link;
    friend class Membership;
    friend class Scope;
    friend class Control;
    friend void changed(Key) noexcept;

    libk::intrusive_tree_hook id_hook_;
    struct Compare {
        bool operator()(const anchor& a, const anchor& b) const noexcept { return a.id_.value < b.id_.value; }
        bool operator()(u64 a, const anchor& b) const noexcept { return a < b.id_.value; }
        bool operator()(const anchor& a, u64 b) const noexcept { return a.id_.value < b; }
    };
    using Index = libk::intrusive_tree<anchor, &anchor::id_hook_, Compare>;
    static libk::manual_lifetime<Index> ids_;
    void index(bool insert) noexcept;
    void* owner_{};
    const Ops* ops_{};
    u64 generation_{};
    Id id_{};
    Gate gate_{};
    Membership* membership_{};
    std::atomic<usize> refs_{};
    ObjKind kind_{ObjInvalid};
    phase phase_{phase::free};
    bool queued_{};
    bool scanning_{};
};

// Move-only ownership of the pool cleanup reference. Synchronous objects
// complete it in their retire hook; asynchronous objects retain it until all
// subsystem cleanup (including hardware retirement) is actually complete.
// It changes only the anchor's canonical cleanup gate and never creates a
// second payload lifecycle counter.
class cleanup final : private libk::noncopyable {
public:
    cleanup() noexcept = default;

    cleanup(cleanup&& other) noexcept
        : anchor_(std::exchange(other.anchor_, nullptr)),
          generation_(std::exchange(other.generation_, u64{})) {}

    auto operator=(cleanup&& other) noexcept -> cleanup& {
        if (this != &other) {
            libk_assert(anchor_ == nullptr);
            anchor_ = std::exchange(other.anchor_, nullptr);
            generation_ = std::exchange(other.generation_, u64{});
        }
        return *this;
    }

    ~cleanup() noexcept { libk_assert(anchor_ == nullptr); }

    [[nodiscard]] explicit operator bool() const noexcept {
        return anchor_ != nullptr;
    }

    void complete() noexcept {
        anchor* const anchor = std::exchange(anchor_, nullptr);
        const u64 generation = std::exchange(generation_, u64{});
        libk_assert(anchor != nullptr);
        anchor->ops_->finish_cleanup(
            anchor->owner_, *anchor, generation);
    }

private:
    template<typename T>
    friend class pool;

    cleanup(anchor& anchor, u64 generation) noexcept
        : anchor_(&anchor), generation_(generation) {}

    anchor* anchor_{};
    u64 generation_{};
};

// A reference protects storage, not permission or operation admission. New
// Weak lookup requires Live; retirement may run while existing owners remain.
// Payload methods and capability pins enforce their own operation contract.
template<typename T>
class ref final {
public:
    // A known live caller owns the storage already. This only transfers
    // execution lifetime, including during cleanup; it grants no new use.
    static auto retain_alive(Key key) noexcept -> ref {
        if (!key.valid()) return {};
        auto& a = *reinterpret_cast<anchor*>(key.slot);
        a.ops_->retain(a.owner_, a, key.generation);
        return ref{a, key.generation};
    }
    ref() noexcept = default;
    ref(const ref& other) noexcept : anchor_(other.anchor_), generation_(other.generation_) {
        retain();
    }
    template<typename U>
        requires (std::is_void_v<T> && !std::is_void_v<U>)
    ref(const ref<U>& other) noexcept : anchor_(other.anchor_), generation_(other.generation_) {
        retain();
    }
    auto operator=(const ref& other) noexcept -> ref& { return *this = ref{other}; }

    ref(ref&& other) noexcept
        : anchor_(std::exchange(other.anchor_, nullptr)),
          generation_(std::exchange(other.generation_, u64{})) {}

    // Type erasure transfers an existing owner, including during retirement.
    // It neither admits a new operation nor acquires another reference.
    template<typename U>
        requires (std::is_void_v<T> && !std::is_void_v<U>)
    ref(ref<U>&& other) noexcept
        : anchor_(std::exchange(other.anchor_, nullptr)),
          generation_(std::exchange(other.generation_, u64{})) {}

    auto operator=(ref&& other) noexcept -> ref& {
        if (this != &other) {
            reset();
            anchor_ = std::exchange(other.anchor_, nullptr);
            generation_ = std::exchange(other.generation_, u64{});
        }
        return *this;
    }
    ~ref() noexcept { reset(); }

    [[nodiscard]] explicit operator bool() const noexcept { return anchor_ != nullptr; }
    [[nodiscard]] auto live() const noexcept -> bool {
        return anchor_ && anchor_->ops_->live(anchor_->owner_, *anchor_, generation_);
    }
    [[nodiscard]] auto kind() const noexcept -> ObjKind {
        return anchor_ != nullptr ? anchor_->kind_ : ObjInvalid;
    }
    [[nodiscard]] auto id() const noexcept -> Id { return anchor_ ? anchor_->id_ : Id{}; }
    auto storage() const noexcept -> pool<T>& requires (!std::is_void_v<T>) {
        return *static_cast<pool<T>*>(anchor_->owner_);
    }
    [[nodiscard]] auto key() const noexcept -> Key {
        return anchor_ != nullptr ? Key{
            .slot = reinterpret_cast<usize>(anchor_),
            .generation = generation_} : Key{};
    }

    [[nodiscard]] auto admit() const noexcept -> pin;
    // The caller already owns this storage (directly or through a real Link).
    static auto admitting(Key key) noexcept -> bool {
        if (!key.valid()) return false;
        const auto& owner = *reinterpret_cast<const anchor*>(key.slot);
        return owner.generation_ == key.generation && owner.gate_.open();
    }
    auto membership() const noexcept -> Membership* { return anchor_ ? anchor_->membership_ : nullptr; }
    template<class Commit> auto publish(Commit&& commit) const noexcept -> bool {
        return anchor_ && anchor_->gate_.publish(std::forward<Commit>(commit));
    }
    // Caller owns the target refs through commit, including after moving caps.
    // A batch locks each distinct object once, in stable identity order.
    template<class Commit>
    static auto publish(std::span<const ref*> targets, Commit&& commit) noexcept -> bool {
        std::ranges::sort(targets, {}, [](const ref* target) { return target->id().value; });
        auto end = std::ranges::unique(targets, {}, [](const ref* target) { return target->id(); }).begin();
        targets = targets.first(static_cast<usize>(end - targets.begin()));
        sync::Irq irq;
        for (const auto* target : targets) target->anchor_->gate_.lock_.lock();
        const bool open = std::ranges::all_of(targets, [](const ref* target) { return target->anchor_->gate_.open(); });
        if (open) std::forward<Commit>(commit)();
        for (usize i = targets.size(); i != 0; --i) targets[i - 1]->anchor_->gate_.lock_.unlock();
        return open;
    }

    // An erased owner exposes the payload with the same lifetime as this ref.
    [[nodiscard]] auto get() const noexcept -> void* requires (std::is_void_v<T>) {
        libk_assert(anchor_ != nullptr);
        return anchor_->ops_->payload(*anchor_);
    }
    [[nodiscard]] decltype(auto) get() noexcept requires (!std::is_void_v<T>) {
        libk_assert(anchor_ != nullptr);
        return *static_cast<T*>(anchor_->ops_->payload(*anchor_));
    }
    [[nodiscard]] decltype(auto) get() const noexcept requires (!std::is_void_v<T>) {
        libk_assert(anchor_ != nullptr);
        return *static_cast<const T*>(anchor_->ops_->payload(*anchor_));
    }
    [[nodiscard]] auto operator->() noexcept -> T* requires (!std::is_void_v<T>) { return &get(); }
    [[nodiscard]] auto operator->() const noexcept -> const T* requires (!std::is_void_v<T>) { return &get(); }

    template<typename U>
    [[nodiscard]] auto as() const & noexcept -> std::expected<ref<U>, error> {
        static_assert(object::kind<U> != ObjInvalid);
        if (!anchor_) return std::unexpected(error::invalid_id);
        if (anchor_->kind_ != object::kind<U>) return std::unexpected(error::wrong_type);
        anchor_->ops_->retain(anchor_->owner_, *anchor_, generation_);
        return ref<U>{*anchor_, generation_};
    }
    template<typename U>
    [[nodiscard]] auto as() && noexcept -> std::expected<ref<U>, error> {
        static_assert(object::kind<U> != ObjInvalid);
        if (!anchor_) return std::unexpected(error::invalid_id);
        if (anchor_->kind_ != object::kind<U>) return std::unexpected(error::wrong_type);
        return ref<U>{*std::exchange(anchor_, nullptr), std::exchange(generation_, u64{})};
    }
    // Capability destruction holds an admitted grant operation through this
    // call. It closes the slot's group member, or retires an unmanaged object.
    [[nodiscard]] auto destroy() const noexcept -> bool {
        return anchor_ && anchor_->ops_->destroy(anchor_->owner_, *anchor_, generation_);
    }
    auto allocation() const noexcept -> resource::Allocation* { return anchor_ ? anchor_->ops_->allocation(*anchor_) : nullptr; }
    auto control() const noexcept -> Control* { return anchor_ ? anchor_->ops_->control(*anchor_) : nullptr; }
    void reset() noexcept {
        auto* anchor = std::exchange(anchor_, nullptr);
        const auto generation = std::exchange(generation_, u64{});
        if (anchor) anchor->ops_->drop_ref(anchor->owner_, *anchor, generation);
    }

private:
    friend class Membership;
    friend class Scope;
    [[nodiscard]] auto retire() const noexcept -> bool {
        return anchor_ != nullptr && anchor_->ops_->request_retire(anchor_->owner_, *anchor_, generation_);
    }
    template<typename> friend class ref;
    template<typename> friend class pool;
    ref(anchor& anchor, u64 generation) noexcept : anchor_(&anchor), generation_(generation) {}

    void retain() const noexcept {
        if (anchor_) anchor_->ops_->retain(anchor_->owner_, *anchor_, generation_);
    }
    anchor* anchor_{};
    u64 generation_{};
};

// Accepted use owns a Gate pin, independently from ordinary storage refs.
// If the final Ref leaves first, the pool's cleanup hold protects this token
// and delays payload retirement until the last accepted use has returned.
class pin final : private libk::noncopyable {
    anchor* anchor_{};
    u64 generation_{};
    template<class T> friend class ref;
    friend class Link;
    friend class Gate;
    pin(anchor& a, u64 generation) noexcept : anchor_(&a), generation_(generation) {}
public:
    pin() = default;
    pin(pin&& other) noexcept : anchor_(std::exchange(other.anchor_, nullptr)), generation_(other.generation_) {}
    auto operator=(pin&& other) noexcept -> pin& {
        if (this != &other) { reset(); anchor_ = std::exchange(other.anchor_, nullptr); generation_ = other.generation_; }
        return *this;
    }
    ~pin() noexcept { reset(); }
    explicit operator bool() const noexcept { return anchor_ != nullptr; }
    template<class Commit> auto publish(Commit&& commit) const noexcept -> bool {
        return anchor_ && anchor_->gate_.publish(std::forward<Commit>(commit));
    }
    void reset() noexcept {
        if (auto* a = std::exchange(anchor_, nullptr)) a->ops_->release_use(a->owner_, *a, generation_);
    }
};

template<class T> auto ref<T>::admit() const noexcept -> pin {
    return anchor_ && anchor_->gate_.enter() ? pin{*anchor_, generation_} : pin{};
}

class LinkWork final : private libk::noncopyable {
    Link* link_{};
    bool final_release_{};
    friend class Link;
    friend class Gate;
    explicit LinkWork(Link& link, bool final_release = false) noexcept : link_(&link), final_release_(final_release) {}
public:
    LinkWork() noexcept = default;
    LinkWork(LinkWork&& other) noexcept : link_(std::exchange(other.link_, nullptr)), final_release_(other.final_release_) {}
    auto operator=(LinkWork&& other) noexcept -> LinkWork& {
        if (this != &other) { reset(); link_ = std::exchange(other.link_, nullptr); final_release_ = other.final_release_; }
        return *this;
    }
    ~LinkWork() noexcept { reset(); }
    explicit operator bool() const noexcept { return link_ != nullptr; }
    void reset() noexcept;
};

// One installed relationship participates in both its authority index and
// its target's close index. Callback work and hardware drain retain its pin.
class Link final : private libk::noncopyable_nonmovable {
    enum class State : u8 { Idle, Live, Invalidating, Draining, Releasing, Detached };
    friend class Gate;
    friend class LinkWork;
    Gate* gate_{};
    anchor* anchor_{};
    Link* previous_{};
    Link* next_{};
    identity<Link> id_{};
    Id target_{};
    pin use_;
    void* context_{};
    void (*invalidate_)(void*, LinkWork&&) noexcept;
    void (*released_)(void*, LinkWork&&) noexcept;
    std::atomic<usize> work_{};
    std::atomic<State> state_{State::Idle};
    void drop_work() noexcept {
        bool release{};
        {
            sync::Lock guard{gate_->lock_};
            const auto count = work_.load(std::memory_order_relaxed);
            libk_assert(count != 0);
            release = count == 1 && state_.load(std::memory_order_relaxed) == State::Draining;
            if (release) state_.store(State::Releasing, std::memory_order_release);
            else work_.store(count - 1, std::memory_order_release);
        }
        if (!release) return; // No further access after publishing zero.
        LinkWork done{*this, true};
        released_(context_, std::move(done));
    }
    void finish_work() noexcept {
        pin use;
        {
            sync::Lock guard{gate_->lock_};
            libk_assert(state_.load() == State::Releasing && work_.load() == 1);
            gate_ = nullptr;
            anchor_ = nullptr;
            use = std::move(use_);
            work_.store(0, std::memory_order_release);
            state_.store(State::Detached, std::memory_order_release);
        }
    }
public:
    Link(void* context, void (*invalidate)(void*, LinkWork&&) noexcept,
         void (*released)(void*, LinkWork&&) noexcept) noexcept
        : context_(context), invalidate_(invalidate), released_(released) {}
    ~Link() noexcept {
        libk_assert(!gate_ && !use_ && !busy());
        libk_assert(state_.load() == State::Idle || state_.load() == State::Detached);
    }
    auto id() const noexcept -> identity<Link> { return id_; }
    auto target_id() const noexcept -> Id { return target_; }
    auto phase() const noexcept -> u32 {
        switch (state_.load(std::memory_order_acquire)) {
        case State::Live: return LINK_LIVE;
        case State::Invalidating: return LINK_INVALIDATING;
        default: return LINK_DRAINING;
        }
    }
    // Caller serializes relation detach/reuse; the Gate synchronizes invalidation.
    auto reference() const noexcept -> ref<> {
        if (!gate_ || !use_) return {};
        // The accepted pin protects storage while its owner excludes detach.
        // Pool retain precedes the Gate lock, matching retirement's lock order.
        auto storage = ref<>::retain_alive({reinterpret_cast<usize>(anchor_), anchor_->generation_});
        {
            sync::Lock guard{gate_->lock_};
            if (state_.load(std::memory_order_relaxed) != State::Live) return {};
        }
        return storage;
    }
    auto attached() const noexcept -> bool {
        const auto state = state_.load(std::memory_order_acquire);
        return state == State::Live || state == State::Invalidating;
    }
    auto busy() const noexcept -> bool {
        const auto state = state_.load(std::memory_order_acquire);
        return state == State::Draining || state == State::Releasing
            || work_.load(std::memory_order_acquire) != 0;
    }
    class Prepared final : private libk::noncopyable {
        friend class Link;
        pin use_;
        identity<Link> id_;
        Id target_;
        bool observe_{};
        Prepared(pin&& use, identity<Link> id, Id target, bool observe) noexcept
            : use_(std::move(use)), id_(id), target_(target), observe_(observe) {}
    public:
        Prepared() noexcept = default;
        Prepared(Prepared&&) noexcept = default;
        auto operator=(Prepared&&) noexcept -> Prepared& = default;
    };
    // Prepare every fallible resource before a batch takes publication Gates.
    auto prepare(const ref<>& target, bool observe = false) noexcept -> std::optional<Prepared> {
        if (state_.load(std::memory_order_acquire) != State::Idle) return {};
        const auto id = new_id<Link>();
        auto use = target.admit();
        if (!id.valid() || !use) return {};
        return Prepared{std::move(use), id, target.id(), observe};
    }
    void install(Prepared& prepared) noexcept {
        libk_assert(state_.load(std::memory_order_relaxed) == State::Idle && prepared.use_ && !gate_ && !use_);
        auto& gate = prepared.use_.anchor_->gate_;
        libk_assert(gate.lock_.held() && gate.open());
        id_ = prepared.id_; target_ = prepared.target_;
        anchor_ = prepared.use_.anchor_;
        if (!prepared.observe_) use_ = std::move(prepared.use_);
        gate_ = &gate;
        next_ = gate.links_;
        if (next_) next_->previous_ = this;
        gate.links_ = this;
        state_.store(State::Live, std::memory_order_release);
    }
    template<class Commit> auto attach(const ref<>& target, Commit&& commit, bool observe = false) noexcept -> bool {
        return attach(target, std::forward<Commit>(commit), observe,
            [&](auto&& install) { return target.publish(std::forward<decltype(install)>(install)); });
    }
    template<class Commit, class Publish>
    auto attach(const ref<>& target, Commit&& commit, bool observe, Publish&& publish) noexcept -> bool {
        auto prepared = prepare(target, observe);
        if (!prepared) return false;
        return publish([&] { install(*prepared); std::forward<Commit>(commit)(); });
    }
    auto claim() noexcept -> LinkWork {
        if (!gate_) return {};
        sync::Lock guard{gate_->lock_};
        if (state_.load(std::memory_order_relaxed) != State::Live) return {};
        retain_cleanup();
        gate_->unlink(*this);
        work_.fetch_add(1, std::memory_order_relaxed);
        state_.store(State::Invalidating, std::memory_order_release);
        return LinkWork{*this};
    }
    void invalidate(LinkWork&& work) noexcept {
        {
            sync::Lock guard{gate_->lock_};
            libk_assert(work_.load() != 0);
            work_.fetch_add(1, std::memory_order_relaxed);
        }
        LinkWork running{*this};
        invalidate_(context_, std::move(work));
    }
    auto detach() noexcept -> bool {
        pin use;
        bool quiescent{};
        {
            const auto current = state_.load(std::memory_order_acquire);
            if (current == State::Detached) return true;
            if (current == State::Idle || current == State::Draining || current == State::Releasing) return false;
            sync::Lock guard{gate_->lock_};
            const auto state = state_.load(std::memory_order_relaxed);
            if (state == State::Draining || state == State::Releasing) return false;
            if (state == State::Live) gate_->unlink(*this);
            quiescent = work_.load(std::memory_order_relaxed) == 0;
            if (quiescent) { gate_ = nullptr; anchor_ = nullptr; use = std::move(use_); }
            state_.store(quiescent ? State::Detached : State::Draining, std::memory_order_release);
        }
        return quiescent;
    }
    void reset() noexcept {
        libk_assert(!gate_ && !use_ && !busy());
        libk_assert(state_.load() == State::Idle || state_.load() == State::Detached);
        id_ = {}; target_ = {};
        state_.store(State::Idle, std::memory_order_release);
    }
private:
    void retain_cleanup() noexcept {
        libk_assert(gate_->lock_.held());
        if (use_) return;
        const auto state = gate_->state_.fetch_add(1, std::memory_order_acq_rel);
        libk_assert((state & ~Gate::closing) < Gate::closing - 1);
        use_ = pin{*anchor_, anchor_->generation_};
    }
};

inline void LinkWork::reset() noexcept {
    if (auto* link = std::exchange(link_, nullptr)) {
        if (final_release_) link->finish_work();
        else link->drop_work();
    }
}

inline void Gate::unlink(Link& link) noexcept {
    libk_assert(lock_.held());
    if (link.previous_) link.previous_->next_ = link.next_;
    else { libk_assert(links_ == &link); links_ = link.next_; }
    if (link.next_) link.next_->previous_ = link.previous_;
    link.previous_ = link.next_ = nullptr;
}

inline auto Gate::invalidate(usize limit) noexcept -> bool {
    for (usize step = 0; step < limit; ++step) {
        Link* link{};
        LinkWork work;
        {
            sync::Lock guard{lock_};
            libk_assert(!open());
            if (!links_) return true;
            link = links_;
            link->retain_cleanup();
            unlink(*link);
            link->work_.fetch_add(1, std::memory_order_relaxed);
            link->state_.store(Link::State::Invalidating, std::memory_order_release);
            work = LinkWork{*link};
        }
        link->invalidate(std::move(work));
    }
    sync::Lock guard{lock_};
    return !links_;
}

} // namespace object
