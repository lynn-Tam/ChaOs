#pragma once

#include <cap/authority.hpp>
#include <core/types.hpp>
#include <libk/expected.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/noncopyable.hpp>
#include <libk/sync/atomic.hpp>
#include <object/object_id.hpp>
#include <object/object_ref.hpp>
#include <sync/completion.hpp>
#include <thread/wait.hpp>

namespace kernel::cap {

class GrantGraph;
class CSpace;
class GrantAttachment;
class GrantRef;
class GrantRevokeWait;

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

// Embedded in a VMM-facing MappingAuthority. GrantGraph indexes it
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
    [[nodiscard]] auto detach() noexcept -> bool;

private:
    friend class GrantGraph;
    friend class GrantWork;

    enum class State : u8 {
        Idle,
        Attached,
        Invalidating,
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

struct GrantKey final {
    usize slot{};
    u64 generation{};

    [[nodiscard]] constexpr auto valid() const noexcept -> bool {
        return slot != 0 && generation != 0;
    }
    [[nodiscard]] friend constexpr auto operator==(
        GrantKey, GrantKey) noexcept -> bool = default;
};

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
    GrantLease(GrantLease&& other) noexcept;
    auto operator=(GrantLease&& other) noexcept -> GrantLease&;
    ~GrantLease() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept {
        return graph_ != nullptr;
    }
    [[nodiscard]] auto key() const noexcept -> GrantKey;
    [[nodiscard]] auto kind() const noexcept -> object::ObjectKind;
    [[nodiscard]] auto ceiling() const noexcept -> GrantCeiling;
    [[nodiscard]] auto clone_target() const noexcept
        -> libk::Expected<object::ObjectRef, object::ObjectError>;
    [[nodiscard]] auto attach(GrantAttachment& attachment) const noexcept
        -> libk::Expected<void, GrantError>;
    [[nodiscard]] auto derive_region(
        object::ObjectRef&& target,
        GrantCeiling ceiling,
        RegionDerivation proof) const noexcept
        -> libk::Expected<GrantRef, GrantError>;
    void reset() noexcept;

private:
    friend class GrantGraph;
    GrantLease(
        GrantGraph& graph,
        void* node,
        u64 generation) noexcept
        : graph_(&graph), node_(node), generation_(generation) {}

    GrantGraph* graph_{};
    void* node_{};
    u64 generation_{};
};

class GrantRef final : private libk::noncopyable {
public:
    GrantRef() noexcept = default;
    GrantRef(GrantRef&& other) noexcept;
    auto operator=(GrantRef&& other) noexcept -> GrantRef&;
    ~GrantRef() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept {
        return graph_ != nullptr;
    }
    [[nodiscard]] auto key() const noexcept -> GrantKey;
    [[nodiscard]] auto graph() const noexcept -> GrantGraph&;
    [[nodiscard]] auto clone() const noexcept
        -> libk::Expected<GrantRef, GrantError>;
    [[nodiscard]] auto acquire() const noexcept
        -> libk::Expected<GrantLease, GrantError>;
    void reset() noexcept;

private:
    friend class GrantGraph;
    GrantRef(
        GrantGraph& graph,
        void* node,
        u64 generation) noexcept
        : graph_(&graph), node_(node), generation_(generation) {}

    GrantGraph* graph_{};
    void* node_{};
    u64 generation_{};
};

// Caller-owned completion for one lineage transition. Existing operation
// leases may finish, but the ticket cannot disappear while nodes reference it.
class GrantRevoke final : private libk::noncopyable_nonmovable {
public:
    explicit GrantRevoke(
        kernel::sync::Completion::Notifier notifier = {}) noexcept
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

    kernel::sync::Completion completion_;
};

// Capability-owned operation used only for a blocking invocation. GrantGraph
// owns its stable storage; Thread observes it through the embedded generic
// WaitRelation and never contains capability-specific state.
class GrantRevokeWait final : private libk::noncopyable_nonmovable {
public:
    explicit GrantRevokeWait(GrantGraph& graph) noexcept;
    ~GrantRevokeWait() noexcept = default;

    [[nodiscard]] auto complete() const noexcept -> bool {
        return completion_.complete();
    }
    [[nodiscard]] auto arm() noexcept -> bool { return completion_.arm(); }
    [[nodiscard]] auto relation() noexcept -> kernel::WaitRelation& {
        return relation_;
    }

private:
    friend class CSpace;
    friend class GrantGraph;

    void ready() noexcept;
    void resume(arch::TrapContext&) noexcept;
    void cancel() noexcept;

    GrantGraph* graph_{};
    GrantRevoke completion_;
    kernel::WaitRelation relation_;
};

} // namespace kernel::cap
