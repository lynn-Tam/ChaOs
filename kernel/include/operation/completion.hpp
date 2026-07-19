#pragma once

#include <arch/trap.hpp>
#include <core/types.hpp>
#include <libk/noncopyable.hpp>
#include <libk/sync/atomic.hpp>
#include <libk/variant.hpp>
#include <operation/key.hpp>
#include <uapi/status.h>

namespace kernel {

class CpuRegistry;
class Vproc;

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
        void (*resume)(void*, arch::TrapContext&) noexcept{};
    };

    template<typename Owner>
    explicit Completion(Owner& owner, const Ops& ops) noexcept
        : owner_(&owner), ops_(&ops) {}

    // Called only by Wait::begin after its side of the edge is initialized and
    // while the Wait lock is held. This function must not call back into Wait.
    void attach(Wait& wait) noexcept;
    void attach(
        Vproc& vproc,
        CpuRegistry& cpus,
        operation::Key key) noexcept;
    void finish(arch::TrapContext& trap) noexcept;
    // Returns true when this call detached and drained the operation. False
    // means completion publication already owns the edge or the operation is
    // committed and must reach its normal terminal event.
    [[nodiscard]] auto cancel() noexcept -> bool;
    void detach() noexcept;

    enum class Delivery : u8 {
        Detached,
        Attached,
        Claimed,
        Ready,
    };

    void* owner_{};
    const Ops* ops_{};
    struct BlockingSink final {
        Wait* wait{};
    };
    struct VprocSink final {
        Vproc* vproc{};
        CpuRegistry* cpus{};
        operation::Key key{};
    };
    using Sink = libk::variant<libk::monostate, BlockingSink, VprocSink>;

    Sink sink_{};
    libk::Atomic<Delivery> delivery_{Delivery::Detached};
};

} // namespace operation
} // namespace kernel
