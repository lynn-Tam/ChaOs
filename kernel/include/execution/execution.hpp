#pragma once

#include <arch/context.hpp>
#include <execution/binding.hpp>
#include <execution/frame.hpp>
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
namespace operation {
class Wait;
}

enum class ExecutionState : u8 {
    Prepared,
    Ready,
    Running,
    Throttled,
    Blocked,
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
        return active_ != nullptr ? active_->stack().base() : stack_.base();
    }
    [[nodiscard]] auto stack_top() const noexcept -> usize {
        return active_ != nullptr ? active_->stack().top() : stack_.top();
    }
    [[nodiscard]] auto contains(usize address) const noexcept -> bool {
        if (stack_.contains(address)) {
            return true;
        }
        for (execution::Frame* frame = active_; frame != nullptr;
             frame = frame->previous()) {
            if (frame->stack().contains(address)) {
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] auto context() noexcept -> arch::KernelContext& {
        return context_;
    }
    [[nodiscard]] auto context() const noexcept -> const arch::KernelContext& {
        return context_;
    }
    [[nodiscard]] auto binding() noexcept -> ExecutionBinding& {
        return active_ != nullptr ? active_->binding() : binding_;
    }
    [[nodiscard]] auto binding() const noexcept -> const ExecutionBinding& {
        return active_ != nullptr ? active_->binding() : binding_;
    }
    [[nodiscard]] auto base_binding() noexcept -> ExecutionBinding& {
        return binding_;
    }
    [[nodiscard]] auto base_binding() const noexcept
        -> const ExecutionBinding& { return binding_; }
    [[nodiscard]] auto active_frame() const noexcept -> execution::Frame* {
        return active_;
    }
    [[nodiscard]] auto wait() const noexcept -> operation::Wait* {
        return active_ != nullptr ? &active_->wait() : base_wait_;
    }
    [[nodiscard]] auto ipc_buffer() noexcept -> ipc::Buffer* {
        return active_ != nullptr
            ? active_->ipc_buffer() : binding_.ipc_buffer();
    }
    [[nodiscard]] auto ipc_buffer() const noexcept -> const ipc::Buffer* {
        return active_ != nullptr
            ? active_->ipc_buffer() : binding_.ipc_buffer();
    }
    [[nodiscard]] auto binding_before(
        const execution::Frame& frame) noexcept -> ExecutionBinding& {
        KASSERT(active_ == &frame);
        return frame.previous_ != nullptr
            ? frame.previous_->binding() : binding_;
    }
    [[nodiscard]] auto ipc_before(
        const execution::Frame& frame) noexcept -> ipc::Buffer* {
        KASSERT(active_ == &frame);
        return frame.previous_ != nullptr
            ? frame.previous_->ipc_buffer() : binding_.ipc_buffer();
    }
    [[nodiscard]] auto frame_depth() const noexcept -> usize {
        usize result{};
        for (execution::Frame* frame = active_; frame != nullptr;
             frame = frame->previous()) {
            ++result;
        }
        return result;
    }
    [[nodiscard]] auto cancel_pending() const noexcept -> bool {
        for (execution::Frame* frame = active_; frame != nullptr;
             frame = frame->previous()) {
            if (frame->cancel_pending()) {
                return true;
            }
        }
        return false;
    }

    void push(execution::Frame& frame) noexcept {
        KASSERT(frame.previous_ == nullptr);
        frame.previous_ = active_;
        active_ = &frame;
    }
    void pop(execution::Frame& frame) noexcept {
        KASSERT(active_ == &frame);
        active_ = frame.previous_;
        frame.previous_ = nullptr;
    }
    // A Vproc may temporarily detach one complete Endpoint frame chain while
    // keeping this schedulable lane materialized as its base runtime. The
    // owning Call retains the returned top; Execution owns only the currently
    // materialized projection.
    [[nodiscard]] auto detach_frames() noexcept -> execution::Frame* {
        KASSERT(active_ != nullptr);
        return libk::exchange(active_, nullptr);
    }
    void attach_frames(execution::Frame& top) noexcept {
        KASSERT(active_ == nullptr);
        active_ = &top;
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
    void bind_wait(operation::Wait& wait) noexcept {
        KASSERT(base_wait_ == nullptr);
        base_wait_ = &wait;
    }

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
    execution::Frame* active_{};
    operation::Wait* base_wait_{};
    ExecutionState state_{ExecutionState::Prepared};
    sched::Binding* scheduler_binding_{};
    sched::CpuDispatcher* home_{};
    bool prepared_{};
};

} // namespace kernel
