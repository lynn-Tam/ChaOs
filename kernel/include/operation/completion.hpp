#pragma once

#include <arch/trap.hpp>
#include <core/debug.hpp>
#include <core/types.hpp>
#include <diag/concurrency.hpp>
#include <libk/noncopyable.hpp>
#include <libk/optional.hpp>
#include <libk/sync/atomic.hpp>
#include <libk/variant.hpp>
#include <operation/key.hpp>
#include <time/time.hpp>
#include <uapi/status.h>

namespace kernel {

class CpuRegistry;
class Vproc;
namespace sched {
class Binding;
}

namespace operation {

class Wait;

enum class State : u8 {
    Complete,
    Waiting,
};

struct Result final {
    myos_status_t status{MYOS_STATUS_OK};
    usize value{};
};

// One non-owning edge from an operation-owned result to a kernel-managed
// continuation. The operation owns this storage and supplies a statically
// bound complete/finish/cancel table. Thread owns only its one active edge;
// the home dispatcher remains the sole owner of run-state transitions.
class Completion final : private libk::noncopyable_nonmovable {
public:
    /*luna change: add a private resume disposition for serial rearm, reason: a page continuation must retain its owner without attaching a new Completion inside finish*/
    enum class ResumeResult : u8 {
        Done,
        Rearm,
    };

    template<
        typename Owner,
        bool (Owner::*Complete)() const noexcept,
        Result (Owner::*Read)() noexcept,
        void (Owner::*Release)() noexcept,
        bool (Owner::*Cancel)() noexcept>
    [[nodiscard]] static auto bind(Owner& owner) noexcept -> Completion {
        static constexpr Ops operations{
            .complete = [](const void* context) noexcept {
                return (static_cast<const Owner*>(context)->*Complete)();
            },
            .read = [](void* context) noexcept -> Result {
                return (static_cast<Owner*>(context)->*Read)();
            },
            .release = [](void* context) noexcept {
                (static_cast<Owner*>(context)->*Release)();
            },
            .cancel = [](void* context) noexcept -> bool {
                return (static_cast<Owner*>(context)->*Cancel)();
            },
        };
        return Completion{owner, operations};
    }

    template<
        typename Owner,
        bool (Owner::*Complete)() const noexcept,
        Result (Owner::*Read)() noexcept,
        void (Owner::*Release)() noexcept,
        bool (Owner::*Cancel)() noexcept,
        void (Owner::*Resume)(arch::TrapContext&) noexcept>
    [[nodiscard]] static auto bind_resume(Owner& owner) noexcept
        -> Completion {
        static constexpr Ops operations{
            .complete = [](const void* context) noexcept {
                return (static_cast<const Owner*>(context)->*Complete)();
            },
            .read = [](void* context) noexcept -> Result {
                return (static_cast<Owner*>(context)->*Read)();
            },
            .release = [](void* context) noexcept {
                (static_cast<Owner*>(context)->*Release)();
            },
            .cancel = [](void* context) noexcept -> bool {
                return (static_cast<Owner*>(context)->*Cancel)();
            },
            .resume = [](void* context, arch::TrapContext& trap) noexcept {
                (static_cast<Owner*>(context)->*Resume)(trap);
                return ResumeResult::Done;
            },
        };
        return Completion{owner, operations};
    }

    template<
        typename Owner,
        bool (Owner::*Complete)() const noexcept,
        Result (Owner::*Read)() noexcept,
        void (Owner::*Release)() noexcept,
        bool (Owner::*Cancel)() noexcept,
        ResumeResult (Owner::*Resume)(arch::TrapContext&) noexcept>
    [[nodiscard]] static auto bind_resume(Owner& owner) noexcept
        -> Completion {
        static constexpr Ops operations{
            .complete = [](const void* context) noexcept {
                return (static_cast<const Owner*>(context)->*Complete)();
            },
            .read = [](void* context) noexcept -> Result {
                return (static_cast<Owner*>(context)->*Read)();
            },
            .release = [](void* context) noexcept {
                (static_cast<Owner*>(context)->*Release)();
            },
            .cancel = [](void* context) noexcept -> bool {
                return (static_cast<Owner*>(context)->*Cancel)();
            },
            .resume = [](void* context, arch::TrapContext& trap) noexcept {
                return (static_cast<Owner*>(context)->*Resume)(trap);
            },
        };
        return Completion{owner, operations};
    }

    ~Completion() noexcept;

    [[nodiscard]] auto attached() const noexcept -> bool {
        return delivery_.load<libk::MemoryOrder::Acquire>()
            != Delivery::Detached;
    }
    [[nodiscard]] auto complete() const noexcept -> bool {
        return ops_->complete(owner_);
    }
    [[nodiscard]] auto observation_key() const noexcept
        -> diag::concurrency::ObservationKey {
        return diag::concurrency::ObservationKey{
            observation_key_.load<libk::MemoryOrder::Acquire>()};
    }
    // The operation owns one policy descriptor. A sink says how completion
    // reaches a consumer; it does not own timeout or producer policy.
    void set_policy(diag::concurrency::OperationPolicy policy) noexcept {
        KASSERT(!attached());
        policy_ = policy;
    }
    void set_deadline(
        libk::optional<time::Instant> deadline,
        diag::concurrency::NodeRef timeout_driver = {}) noexcept {
        KASSERT(!attached());
        policy_.deadline = deadline ? deadline->ticks() : 0;
        policy_.expectation = deadline
            ? diag::concurrency::Expectation::DeadlineBound
            : diag::concurrency::Expectation::ExternalUnbounded;
        policy_.deadline_driver = deadline ? timeout_driver
                                           : diag::concurrency::NodeRef{};
        KASSERT(!deadline || policy_.deadline_driver);
    }

    // Publication is narrower than scheduling. It may request a retained wake
    // on the target CPU, but it cannot mutate Thread state itself.
    void signal() noexcept;
private:
    friend class kernel::Vproc;
    friend class Wait;

    struct Ops final {
        bool (*complete)(const void*) noexcept;
        Result (*read)(void*) noexcept;
        void (*release)(void*) noexcept;
        bool (*cancel)(void*) noexcept;
        ResumeResult (*resume)(void*, arch::TrapContext&) noexcept{};
    };

    template<typename Owner>
    explicit Completion(Owner& owner, const Ops& ops) noexcept
        : owner_(&owner), ops_(&ops) {}

    // Called only by Wait::begin after its side of the edge is initialized and
    // while the Wait lock is held. This function must not call back into Wait.
    void attach(Wait& wait, sched::Binding& binding) noexcept;
    void attach(
        Vproc& vproc,
        CpuRegistry& cpus,
        operation::Key key) noexcept;
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
    [[nodiscard]] auto try_claim_cancel() noexcept -> bool;

    enum class CancelResult : u8 {
        Reopen,
        Canceled,
        Completed,
    };

    // Resolve policy only after the container has granted cancellation
    // ownership.  No sink, container, scheduler or owner-release callbacks
    // occur here.
    [[nodiscard]] auto resolve_cancel() noexcept -> CancelResult;
    // A cancellation owner may reopen only when this CAS proves no producer
    // recorded CancelRaced.  The caller restores its container edge before
    // publishing Attached.
    [[nodiscard]] auto try_reopen_cancel() noexcept -> bool;
    [[nodiscard]] auto finish_claimed(arch::TrapContext& trap) noexcept
        -> ResumeResult;
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
    struct BlockingSink final {
        Wait* wait{};
        sched::Binding* binding{};
    };
    struct VprocSink final {
        Vproc* vproc{};
        CpuRegistry* cpus{};
        operation::Key key{};
    };
    using Sink = libk::variant<libk::monostate, BlockingSink, VprocSink>;

    Sink sink_{};
    libk::Atomic<Delivery> delivery_{Delivery::Detached};
    // Shared writers publish only this immutable generation identity.  Each
    // writer borrows it for its update; the terminal Delivery owner exchanges
    // it before releasing owner_ so no member is touched after release.
    libk::Atomic<u64> observation_key_{};
    diag::concurrency::OperationPolicy policy_{};
};

} // namespace operation
} // namespace kernel
