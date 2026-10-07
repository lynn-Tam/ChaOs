#pragma once

#include <libk/assert.hpp>
#include <base/types.hpp>
#include <libk/noncopyable.hpp>
#include <libk/sync/atomic.hpp>
#include <uapi/abi.h>
#include <sync.hpp>

class CpuRegistry;
namespace sched { class Sc; }

class Wait;

struct WaitResult final {
    status_t status{STATUS_OK};
    usize value{};
};

// One non-owning edge from an operation-owned result to a kernel-managed
// continuation. The operation owns this storage and supplies a statically
// bound release/cancel table. Thread owns only its one active edge;
// the home dispatcher remains the sole owner of run-state transitions.
class Completion final : private libk::noncopyable_nonmovable {
public:
    template<typename T,
             void (T::*Drop)() noexcept,
             bool (T::*Cancel)() noexcept>
    [[nodiscard]] static auto bind(T& owner) noexcept -> Completion {
        static constexpr Ops ops{
            .release = [](void* p) noexcept { (static_cast<T*>(p)->*Drop)(); },
            .cancel = [](void* p) noexcept { return (static_cast<T*>(p)->*Cancel)(); },
        };
        return Completion{owner, ops};
    }

    ~Completion() noexcept;

    [[nodiscard]] auto attached() const noexcept -> bool {
        return delivery_.load<libk::MemoryOrder::Acquire>()
            != Delivery::Detached;
    }
    // Publication is narrower than scheduling. It may request a retained wake
    // on the target CPU, but it cannot mutate Thread state itself.
    void signal() noexcept;
private:
    friend class Wait;

    struct Ops final {
        void (*release)(void*) noexcept;
        bool (*cancel)(void*) noexcept;
    };

    template<typename Owner>
    explicit Completion(Owner& owner, const Ops& ops) noexcept
        : owner_(&owner), ops_(&ops) {}

    // Called only by Wait::begin after its side of the edge is initialized and
    // while the Wait lock is held. This function must not call back into Wait.
    void attach(Wait& wait) noexcept;
    // Delivery claims are callback-free and only transfer the Completion's
    // terminal ownership.  Callers hold their container lock while claiming
    // and keep the returned owner live until the corresponding terminal
    // method has completed.
    enum class FinishClaim : u8 {
        Claimed,
        Publishing,
        Unavailable,
    };

    [[nodiscard]] auto try_claim_finish() noexcept -> FinishClaim;
    enum class CancelClaim : u8 { Unavailable, Pending, Published };
    [[nodiscard]] auto try_claim_cancel() noexcept -> CancelClaim;

    enum class CancelResult : u8 {
        Reopen,
        Canceled,
        Completed,
    };

    // Resolve policy only after the container has granted cancellation
    // ownership.  No sink, container, scheduler or owner-release callbacks
    // occur here.
    [[nodiscard]] auto resolve_cancel(CancelClaim claim) noexcept -> CancelResult;
    // A cancellation owner may reopen only when this CAS proves no producer
    // recorded CancelRaced.  The caller restores its container edge before
    // publishing Attached.
    [[nodiscard]] auto try_reopen_cancel() noexcept -> bool;
    void finish_claimed() noexcept;
    // Finalize a cancellation after the container has unlinked its pointer
    // and projection.  This is the sole cancellation drain/release path.
    void finalize_cancel(CancelResult result) noexcept;

    enum class Delivery : u8 {
        Detached,
        Attached,
        // The delivery owner is publishing a completion or consuming the
        // published result.  Cancellation has its own states below so a
        // producer cannot disappear behind an undifferentiated claim.
        Claimed,
        Ready,
        // A cancellation owner temporarily holds the delivery edge.  A
        // producer racing this state records that race in CancelRaced rather
        // than returning with no durable handoff.
        Cancelling,
        CancelRaced,
    };

    void* owner_{};
    const Ops* ops_{};
    Wait* wait_{};
    libk::Atomic<Delivery> delivery_{Delivery::Detached};

};

// One blocking edge owned by a kernel-managed continuation. Thread owns the
// base edge; an Endpoint Activation overrides it during a cross-domain call.
// The operation still owns Completion and the dispatcher still owns run state.
class Wait final : private libk::noncopyable_nonmovable {
public:
    Wait() noexcept;
    ~Wait() noexcept;

    [[nodiscard]] auto attached() const noexcept -> bool;
    [[nodiscard]] auto ready() const noexcept -> bool;
    [[nodiscard]] auto begin(
        Completion& completion,
        CpuRegistry& cpus,
        sched::Sc& binding) noexcept -> bool;
    [[nodiscard]] auto finish() noexcept -> bool;
    [[nodiscard]] auto cancel() noexcept -> bool;

private:
    friend class Completion;

    void wake() noexcept;

    enum class EdgePhase : u8 {
        Detached,
        Attached,
        CancelOwned,
        FinishOwned,
    };

    Completion* completion_{};
    CpuRegistry* cpus_{};
    sched::Sc* binding_{};
    mutable sync::Spin lock_{};
    libk::Atomic<bool> ready_{};
    EdgePhase phase_{EdgePhase::Detached};

};

// A committed release runs to its exact refund/revoke receipt. Cancellation
// may abandon admission, but cannot undo a committed release.
class Receipt final : private libk::noncopyable_nonmovable {
public:
    Receipt() noexcept;
    ~Receipt() noexcept { libk_assert(!committed_ || done()); }
    void commit() noexcept { committed_ = true; }
    void signal() noexcept;
    auto completion() noexcept -> Completion& { return completion_; }

private:
    auto done() const noexcept -> bool { return done_.load<libk::MemoryOrder::Acquire>(); }
    auto cancel() noexcept -> bool { return !committed_; }
    void release() noexcept { libk_assert(!committed_ || done()); }

    libk::Atomic<bool> done_{};
    bool committed_{};
    Completion completion_;
};
