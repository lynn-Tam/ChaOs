#pragma once

#include <arch/context.hpp>
#include <execution/binding.hpp>
#include <libk/noncopyable.hpp>
#include <libk/utility.hpp>
#include <mm/kernel_stack.hpp>
#include <resource/sponsorship.hpp>

namespace kernel {

class Vproc;

namespace sched {
class Binding;
class CpuDispatcher;
class SchedulingContext;
}
namespace execution {
class Target;
}
enum class ExecutionState : u8 {
    Prepared,
    Ready,
    Running,
    Throttled,
    Blocked,
    Parked,
    Exited,
};

// Canonical kernel-owned carrier for one schedulable lane. Thread and Vproc
// add different continuation policy, but neither duplicates the home stack,
// saved kernel context, or effective root binding.
class Execution final : private libk::noncopyable_nonmovable {
public:
    Execution(
        kernel::resource::Charge&& stack_charge,
        KernelStack&& home_stack,
        ExecutionBinding&& binding) noexcept;
    Execution(KernelStack&& home_stack, ExecutionBinding&& binding) noexcept
        : Execution(
              kernel::resource::Charge{},
              libk::move(home_stack),
              libk::move(binding)) {}
    ~Execution() noexcept = default;

    [[nodiscard]] auto stack_base() const noexcept -> usize {
        return stack_.base();
    }
    [[nodiscard]] auto stack_top() const noexcept -> usize {
        return stack_.top();
    }
    [[nodiscard]] auto contains(usize address) const noexcept -> bool {
        return stack_.contains(address);
    }
    [[nodiscard]] auto context() noexcept -> arch::KernelContext& {
        return context_;
    }
    [[nodiscard]] auto context() const noexcept -> const arch::KernelContext& {
        return context_;
    }
    [[nodiscard]] auto binding() noexcept -> ExecutionBinding& {
        return binding_;
    }
    [[nodiscard]] auto binding() const noexcept -> const ExecutionBinding& {
        return binding_;
    }
    [[nodiscard]] auto ipc_buffer() noexcept -> ipc::Buffer* {
        return binding_.ipc_buffer();
    }
    [[nodiscard]] auto ipc_buffer() const noexcept -> const ipc::Buffer* {
        return binding_.ipc_buffer();
    }
    [[nodiscard]] auto state() const noexcept -> ExecutionState {
        return state_;
    }
    [[nodiscard]] auto scheduler_binding() noexcept -> sched::Binding* {
        return scheduler_binding_;
    }
    [[nodiscard]] auto scheduler_binding() const noexcept
        -> const sched::Binding* {
        return scheduler_binding_;
    }

    void prepare(
        arch::ContextEntry entry,
        void* owner,
        usize kernel_stack_top) noexcept;

private:
    friend class Thread;
    friend class Vproc;
    friend class sched::Binding;
    friend class sched::CpuDispatcher;
    friend class sched::SchedulingContext;
    friend class execution::Target;

    // Reverse destruction returns the mapped stack before its capacity charge.
    kernel::resource::Charge stack_charge_{};
    KernelStack stack_;
    arch::KernelContext context_{};
    ExecutionBinding binding_;
    ExecutionState state_{ExecutionState::Prepared};
    sched::Binding* scheduler_binding_{};
    sched::CpuDispatcher* home_{};
    bool prepared_{};
};

} // namespace kernel
