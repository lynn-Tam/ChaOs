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

namespace resource {
class Reservation;
}

namespace mm { class VSpace; }
namespace ipc { class Channel; }

namespace cap {

class GrantGraph;
class CSpace;
class GrantAttachment;
class GrantRef;

enum class GrantInvalidation : u8 {
    Revoke,
};

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
    friend class GrantGraph;
    explicit GrantWork(GrantAttachment& attachment) noexcept
        : attachment_(&attachment) {}

    GrantAttachment* attachment_{};
};

struct GrantAttachmentOps final {
    void (*invalidate)(
        void* context,
        GrantWork&& work,
        GrantInvalidation reason) noexcept;
    void (*released)(void* context) noexcept;
};

// Embedded in a VMM-facing Backing. GrantGraph indexes it
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
    friend class GrantGraph;
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
    GrantGraph* graph_{};
    void* node_{};
    u64 generation_{};
    void* context_{};
    const GrantAttachmentOps* ops_{};
    libk::Atomic<usize> work_{};
    libk::Atomic<u8> state_{static_cast<u8>(State::Idle)};
};

using GrantKey = libk::key<GrantGraph>;

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
    [[nodiscard]] auto graph() const noexcept -> GrantGraph&;
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
    friend class GrantGraph;
    friend class mm::VSpace;
    friend class ipc::Channel;
    // Only the object-owned transaction may change its grant's meaning.
    [[nodiscard]] auto mint(resource::Reservation&&, View) const noexcept
        -> std::expected<GrantRef, GrantError>;
    GrantLease(
        GrantGraph& graph,
        void* node,
        u64 generation) noexcept
        : h_(Data{&graph, node, generation}) {}

    struct Data {
        GrantGraph* graph{};
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
    [[nodiscard]] auto graph() const noexcept -> GrantGraph&;
    [[nodiscard]] auto clone() const noexcept
        -> std::expected<GrantRef, GrantError>;
    [[nodiscard]] auto acquire() const noexcept
        -> std::expected<GrantLease, GrantError>;
    void reset() noexcept { h_.reset(); }

private:
    friend class GrantGraph;
    GrantRef(
        GrantGraph& graph,
        void* slot,
        u64 generation) noexcept
        : h_(Data{&graph, slot, generation}) {}

    struct Data {
        GrantGraph* graph{};
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
    friend class GrantGraph;
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

} // namespace cap
