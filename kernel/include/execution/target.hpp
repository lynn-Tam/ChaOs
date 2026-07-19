#pragma once

#include <execution/execution.hpp>
#include <libk/noncopyable.hpp>
#include <libk/variant.hpp>
#include <object/thread_pool.hpp>
#include <object/vproc_pool.hpp>

namespace kernel::sched {
class Binding;
class CpuDispatcher;
}

namespace kernel::operation {
class Wait;
}

namespace kernel::execution {

class Frame;

// Borrowed view of the closed schedulable target set. It carries no lifetime;
// sched::Binding owns a typed hold while CpuDispatcher borrows the committed
// target. Dispatch is generated from this variant without a vtable.
class Target final {
public:
    Target() noexcept = default;
    explicit Target(Thread& thread) noexcept
        : value_(libk::in_place_type<Thread*>, &thread) {}
    explicit Target(Vproc& vproc) noexcept
        : value_(libk::in_place_type<Vproc*>, &vproc) {}

    [[nodiscard]] explicit operator bool() const noexcept {
        return !libk::holds_alternative<libk::monostate>(value_);
    }
    [[nodiscard]] auto thread() const noexcept -> Thread*;
    [[nodiscard]] auto vproc() const noexcept -> Vproc*;
    [[nodiscard]] auto execution() const noexcept -> Execution&;
    [[nodiscard]] auto stack_base() const noexcept -> usize;
    [[nodiscard]] auto stack_top() const noexcept -> usize;
    [[nodiscard]] auto contains_stack(usize address) const noexcept -> bool;
    [[nodiscard]] auto effective_binding() const noexcept -> ExecutionBinding&;
    [[nodiscard]] auto ipc_buffer() const noexcept -> ipc::Buffer*;
    [[nodiscard]] auto wait() const noexcept -> operation::Wait*;
    [[nodiscard]] auto active_frame() const noexcept -> execution::Frame*;
    [[nodiscard]] auto cancel_pending() const noexcept -> bool;
    [[nodiscard]] auto idle() const noexcept -> bool;
    [[nodiscard]] auto identity() const noexcept -> usize;
    [[nodiscard]] auto stop_deferred() const noexcept -> bool;
    [[nodiscard]] auto stop_requested() const noexcept -> bool;
    [[nodiscard]] auto stop_ready() const noexcept -> bool;
    [[nodiscard]] auto claim_home(sched::CpuDispatcher& home) const noexcept
        -> bool;
    [[nodiscard]] auto owned_by(
        const sched::CpuDispatcher& home) const noexcept -> bool;
    [[nodiscard]] auto try_bind(sched::Binding& binding) const noexcept
        -> bool;
    void clear_binding(sched::Binding& binding) const noexcept;
    void finish_stop() const noexcept;

    [[nodiscard]] friend auto operator==(
        const Target&, const Target&) noexcept -> bool = default;

private:
    using Value = libk::variant<libk::monostate, Thread*, Vproc*>;
    Value value_{};
};

// Owning form used only by SchedulingContext::Binding. Converting to Target
// borrows the same object and never creates a second lifetime owner.
class TargetHold final : private libk::noncopyable {
public:
    TargetHold() noexcept = default;
    explicit TargetHold(object::ThreadHold&& thread) noexcept
        : value_(libk::in_place_type<object::ThreadHold>, libk::move(thread)) {}
    explicit TargetHold(object::VprocHold&& vproc) noexcept
        : value_(libk::in_place_type<object::VprocHold>, libk::move(vproc)) {}
    TargetHold(TargetHold&&) noexcept = default;
    auto operator=(TargetHold&&) noexcept -> TargetHold& = default;

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] auto get() noexcept -> Target;
    [[nodiscard]] auto get() const noexcept -> Target;
    [[nodiscard]] auto reference() const noexcept
        -> libk::Expected<object::ObjectRef, object::ObjectError>;

private:
    using Value = libk::variant<
        libk::monostate, object::ThreadHold, object::VprocHold>;
    Value value_{};
};

} // namespace kernel::execution
