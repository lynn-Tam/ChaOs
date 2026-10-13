#pragma once

#include <libk/assert.hpp>
#include <libk/types.hpp>
#include <expected>
#include <libk/noncopyable.hpp>
#include <utility>
#include <uapi/cap.h>
#include <libk/key.hpp>
#include <type_traits>

namespace ipc { class Endpoint; class Channel; class Notification; }
namespace irq { class Irq; }
namespace io { class Host; class Space; }
namespace sched { class Sc; class Domain; }
namespace cap { class CSpace; }
namespace mm { class Mem; class VSpace; }
class Pager;
class Thread;

namespace object {

class group;
class Member;
class anchor;
// One table binds ABI tags to payload types and their typed storage.
template<class...> class store;
template<class T, ObjKind K> struct Def { using type = T; static constexpr ObjKind kind = K; };
template<class... E> struct Catalog {
    using objects = store<typename E::type...>;
    template<class T> static constexpr ObjKind kind = [] {
        ObjKind k = ObjInvalid;
        ((std::is_same_v<T, typename E::type> ? k = E::kind : k), ...);
        return k;
    }();
};
using Types = Catalog<Def<io::Space, ObjIoSpace>, Def<irq::Irq, ObjIrq>,
    Def<io::Host, ObjHost>, Def<mm::VSpace, ObjVSpace>, Def<mm::Mem, ObjMem>,
    Def<Pager, ObjPager>, Def<cap::CSpace, ObjCSpace>, Def<ipc::Endpoint, ObjEndpoint>,
    Def<ipc::Channel, ObjChannel>, Def<ipc::Notification, ObjNotification>,
    Def<sched::Domain, ObjDomain>, Def<sched::Sc, ObjSc>, Def<Thread, ObjThread>, Def<group, ObjGroup>>;
using Objects = Types::objects;
template<class T> inline constexpr ObjKind kind = Types::kind<T>;
using Id = libk::key<anchor>;

enum class error : u8 {
    out_of_memory,
    exhausted,
    invalid_id,
    closed,
    wrong_type,
};

template<typename T = void>
class ref;
class cleanup;
template<typename T>
class pool;

// Common storage header for every typed pool slot. It is deliberately
// outside the payload: kernel objects do not inherit a lifecycle base and do
// not carry capability state. The ops table returns to the actual pool so all
// reference transitions remain serialized by that pool's one lock.
class anchor final {
private:
    enum class phase : u8 { free, constructing, live, retiring, quiescent };

    struct Ops final {
        void (*retain)(void*, anchor&, u64) noexcept;
        bool (*live)(void*, anchor&, u64) noexcept;
        bool (*try_retain)(void*, anchor&, u64) noexcept;
        void (*drop_ref)(void*, anchor&, u64) noexcept;
        void* (*payload)(anchor&) noexcept;
        bool (*destroy)(void*, anchor&, u64) noexcept;
        bool (*request_retire)(void*, anchor&, u64) noexcept;
        void (*finish_cleanup)(void*, anchor&, u64) noexcept;
    };

    template<typename T>
    friend class pool;
    template<typename T>
    friend class ref;
    friend class cleanup;
    friend class Member;

    void* owner_{};
    const Ops* ops_{};
    u64 generation_{};
    usize refs_{};
    ObjKind kind_{ObjInvalid};
    phase phase_{phase::free};
    bool clean_{};
    bool queued_{};
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
// Payload methods and capability leases enforce their own operation contract.
template<typename T>
class ref final {
public:
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
    [[nodiscard]] auto id() const noexcept -> Id {
        return anchor_ != nullptr ? Id{
            .slot = reinterpret_cast<usize>(anchor_),
            .generation = generation_} : Id{};
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
    void reset() noexcept {
        auto* anchor = std::exchange(anchor_, nullptr);
        const auto generation = std::exchange(generation_, u64{});
        if (anchor) anchor->ops_->drop_ref(anchor->owner_, *anchor, generation);
    }

private:
    friend class Member;
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

} // namespace object
