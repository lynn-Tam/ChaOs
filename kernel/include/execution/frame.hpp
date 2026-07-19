#pragma once

#include <core/types.hpp>
#include <libk/noncopyable.hpp>

namespace arch {
class TrapContext;
}

namespace kernel {

class ExecutionBinding;
class Execution;
class KernelStack;
namespace sched {
class CpuDispatcher;
}

namespace operation {
class Wait;
}
namespace ipc {
class Buffer;
}

namespace execution {

// One stack/root override in a synchronous cross-domain execution chain.
// The owning subsystem supplies storage and lifetime; Execution owns only the
// current top pointer, so effective stack and roots have one derivation path.
class Frame final : private libk::noncopyable_nonmovable {
public:
    using Unwind = void (*)(
        void*, arch::TrapContext&, sched::CpuDispatcher&, isize) noexcept;
    using CancelPending = bool (*)(const void*) noexcept;
    enum class Kind : unsigned char {
        Endpoint,
    };

    Frame(
        KernelStack& stack,
        ExecutionBinding& binding,
        ipc::Buffer* ipc,
        operation::Wait& wait,
        Kind kind,
        void* owner,
        Unwind unwind,
        CancelPending cancel_pending) noexcept
        : stack_(&stack), binding_(&binding), ipc_(ipc), wait_(&wait), owner_(owner),
          unwind_(unwind), cancel_pending_(cancel_pending), kind_(kind) {}

    [[nodiscard]] auto stack() const noexcept -> KernelStack& {
        return *stack_;
    }
    [[nodiscard]] auto binding() const noexcept -> ExecutionBinding& {
        return *binding_;
    }
    [[nodiscard]] auto previous() const noexcept -> Frame* {
        return previous_;
    }
    [[nodiscard]] auto owner() const noexcept -> void* { return owner_; }
    [[nodiscard]] auto wait() const noexcept -> operation::Wait& {
        return *wait_;
    }
    [[nodiscard]] auto ipc_buffer() const noexcept -> ipc::Buffer* {
        return ipc_;
    }
    [[nodiscard]] auto kind() const noexcept -> Kind { return kind_; }
    [[nodiscard]] auto cancel_pending() const noexcept -> bool {
        KASSERT(cancel_pending_ != nullptr);
        return cancel_pending_(owner_);
    }
    void unwind(
        arch::TrapContext& trap,
        sched::CpuDispatcher& dispatcher,
        isize status) noexcept {
        KASSERT(unwind_ != nullptr);
        unwind_(owner_, trap, dispatcher, status);
    }

private:
    friend class ::kernel::Execution;

    KernelStack* stack_{};
    ExecutionBinding* binding_{};
    ipc::Buffer* ipc_{};
    operation::Wait* wait_{};
    void* owner_{};
    Unwind unwind_{};
    CancelPending cancel_pending_{};
    Kind kind_{Kind::Endpoint};
    Frame* previous_{};
};

} // namespace execution
} // namespace kernel
