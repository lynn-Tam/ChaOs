#include <execution/target.hpp>

#include <core/debug.hpp>
#include <execution/vproc.hpp>
#include <sched/binding.hpp>
#include <sync/irq_lock_guard.hpp>
#include <thread/thread.hpp>

namespace kernel::execution {

auto Target::thread() const noexcept -> Thread* {
    auto* const value = libk::get_if<Thread*>(&value_);
    return value != nullptr ? *value : nullptr;
}

auto Target::vproc() const noexcept -> Vproc* {
    auto* const value = libk::get_if<Vproc*>(&value_);
    return value != nullptr ? *value : nullptr;
}

auto Target::execution() const noexcept -> Execution& {
    return libk::visit([](auto value) noexcept -> Execution& {
        using T = libk::remove_cvr_t<decltype(value)>;
        if constexpr (libk::SameAs<T, libk::monostate>) {
            KASSERT(false);
            __builtin_unreachable();
        } else {
            KASSERT(value != nullptr);
            return value->execution();
        }
    }, value_);
}

auto Target::stack_base() const noexcept -> usize {
    return libk::visit([](auto value) noexcept -> usize {
        using T = libk::remove_cvr_t<decltype(value)>;
        if constexpr (libk::SameAs<T, libk::monostate>) {
            KASSERT(false);
            __builtin_unreachable();
        } else if constexpr (libk::SameAs<T, Thread*>) {
            return value->current_stack_base();
        } else {
            return value->execution().stack_base();
        }
    }, value_);
}

auto Target::stack_top() const noexcept -> usize {
    return libk::visit([](auto value) noexcept -> usize {
        using T = libk::remove_cvr_t<decltype(value)>;
        if constexpr (libk::SameAs<T, libk::monostate>) {
            KASSERT(false);
            __builtin_unreachable();
        } else if constexpr (libk::SameAs<T, Thread*>) {
            return value->current_stack_top();
        } else {
            return value->execution().stack_top();
        }
    }, value_);
}

auto Target::contains_stack(usize address) const noexcept -> bool {
    return libk::visit([address](auto value) noexcept -> bool {
        using T = libk::remove_cvr_t<decltype(value)>;
        if constexpr (libk::SameAs<T, libk::monostate>) {
            return false;
        } else if constexpr (libk::SameAs<T, Thread*>) {
            return value->contains_stack(address);
        } else {
            return value->execution().contains(address);
        }
    }, value_);
}

auto Target::effective_binding() const noexcept -> ExecutionBinding& {
    return libk::visit([](auto value) noexcept -> ExecutionBinding& {
        using T = libk::remove_cvr_t<decltype(value)>;
        if constexpr (libk::SameAs<T, libk::monostate>) {
            KASSERT(false);
            __builtin_unreachable();
        } else if constexpr (libk::SameAs<T, Thread*>) {
            return value->effective_binding();
        } else {
            return value->execution().binding();
        }
    }, value_);
}

auto Target::ipc_buffer() const noexcept -> ipc::Buffer* {
    return libk::visit([](auto value) noexcept -> ipc::Buffer* {
        using T = libk::remove_cvr_t<decltype(value)>;
        if constexpr (libk::SameAs<T, libk::monostate>) {
            return nullptr;
        } else if constexpr (libk::SameAs<T, Thread*>) {
            return value->ipc_buffer();
        } else {
            return value->execution().ipc_buffer();
        }
    }, value_);
}

auto Target::wait() const noexcept -> operation::Wait* {
    Thread* const value = thread();
    return value != nullptr ? &value->current_wait() : nullptr;
}

auto Target::active_frame() const noexcept -> execution::Frame* {
    Thread* const value = thread();
    return value != nullptr ? value->active_frame() : nullptr;
}

auto Target::cancel_pending() const noexcept -> bool {
    Thread* const value = thread();
    return value != nullptr && value->cancel_pending();
}

auto Target::idle() const noexcept -> bool {
    Thread* const value = thread();
    return value != nullptr && value->idle();
}

auto Target::identity() const noexcept -> usize {
    return libk::visit([](auto value) noexcept -> usize {
        using T = libk::remove_cvr_t<decltype(value)>;
        if constexpr (libk::SameAs<T, libk::monostate>) {
            return 0;
        } else {
            return reinterpret_cast<usize>(value);
        }
    }, value_);
}

auto Target::stop_deferred() const noexcept -> bool {
    if (Thread* const value = thread()) {
        return value->active_frame() != nullptr
            || value->current_wait().attached();
    }
    Vproc* const value = vproc();
    return value != nullptr
        && (value->pending_operations() || !value->activation_quiescent());
}

auto Target::stop_requested() const noexcept -> bool {
    return libk::visit([](auto value) noexcept -> bool {
        using T = libk::remove_cvr_t<decltype(value)>;
        if constexpr (libk::SameAs<T, libk::monostate>) {
            return true;
        } else if constexpr (libk::SameAs<T, Vproc*>) {
            kernel::sync::IrqLockGuard guard{value->state_lock_};
            return value->stop_requested_ || value->stopped_;
        } else {
            kernel::sync::IrqLockGuard guard{value->stop_lock_};
            return value->stop_requested_ || value->stopped_;
        }
    }, value_);
}

auto Target::stop_ready() const noexcept -> bool {
    return libk::visit([](auto value) noexcept -> bool {
        using T = libk::remove_cvr_t<decltype(value)>;
        if constexpr (libk::SameAs<T, libk::monostate>) {
            return false;
        } else if constexpr (libk::SameAs<T, Vproc*>) {
            kernel::sync::IrqLockGuard guard{value->state_lock_};
            if (!value->stop_requested_) {
                return false;
            }
            for (const auto& slot : value->operations_) {
                if (slot.state == Vproc::OperationState::Pending) {
                    return false;
                }
            }
            return value->activation_publishers_ == 0
                && !value->activation_request_held_
                && !value->activation_posting_
                && !value->activation_.pending();
        } else {
            kernel::sync::IrqLockGuard guard{value->stop_lock_};
            return value->stop_requested_
                && value->active_frame() == nullptr
                && !value->current_wait().attached();
        }
    }, value_);
}

auto Target::claim_home(sched::CpuDispatcher& home) const noexcept -> bool {
    return libk::visit([&home](auto value) noexcept -> bool {
        using T = libk::remove_cvr_t<decltype(value)>;
        if constexpr (libk::SameAs<T, libk::monostate>) {
            return false;
        } else if constexpr (libk::SameAs<T, Vproc*>) {
            kernel::sync::IrqLockGuard guard{value->state_lock_};
            Execution& execution = value->execution_;
            if (execution.home_ != nullptr && execution.home_ != &home) {
                return false;
            }
            if (value->stop_requested_ && execution.home_ == nullptr) {
                return false;
            }
            execution.home_ = &home;
            return true;
        } else {
            kernel::sync::IrqLockGuard guard{value->stop_lock_};
            Execution& execution = value->execution_;
            if (execution.home_ != nullptr && execution.home_ != &home) {
                return false;
            }
            if (value->stop_requested_ && execution.home_ == nullptr) {
                return false;
            }
            execution.home_ = &home;
            return true;
        }
    }, value_);
}

auto Target::owned_by(const sched::CpuDispatcher& home) const noexcept -> bool {
    return libk::visit([&home](auto value) noexcept -> bool {
        using T = libk::remove_cvr_t<decltype(value)>;
        if constexpr (libk::SameAs<T, libk::monostate>) {
            return false;
        } else if constexpr (libk::SameAs<T, Vproc*>) {
            kernel::sync::IrqLockGuard guard{value->state_lock_};
            return value->stop_requested_ && value->execution_.home_ == &home;
        } else {
            kernel::sync::IrqLockGuard guard{value->stop_lock_};
            return value->stop_requested_ && value->execution_.home_ == &home;
        }
    }, value_);
}

auto Target::try_bind(sched::Binding& binding) const noexcept -> bool {
    return libk::visit([&binding](auto value) noexcept -> bool {
        using T = libk::remove_cvr_t<decltype(value)>;
        if constexpr (libk::SameAs<T, libk::monostate>) {
            return false;
        } else if constexpr (libk::SameAs<T, Vproc*>) {
            kernel::sync::IrqLockGuard guard{value->state_lock_};
            Execution& execution = value->execution_;
            if (execution.state_ != ExecutionState::Prepared
                || execution.scheduler_binding_ != nullptr
                || execution.home_ != nullptr || value->stop_requested_
                || value->stopped_) {
                return false;
            }
            execution.scheduler_binding_ = &binding;
            return true;
        } else {
            kernel::sync::IrqLockGuard guard{value->stop_lock_};
            Execution& execution = value->execution_;
            if constexpr (libk::SameAs<T, Thread*>) {
                if (value->idle()) {
                    return false;
                }
            }
            if (execution.state_ != ExecutionState::Prepared
                || execution.scheduler_binding_ != nullptr
                || execution.home_ != nullptr
                || value->stop_requested_ || value->stopped_) {
                return false;
            }
            execution.scheduler_binding_ = &binding;
            return true;
        }
    }, value_);
}

void Target::clear_binding(sched::Binding& binding) const noexcept {
    libk::visit([&binding](auto value) noexcept {
        using T = libk::remove_cvr_t<decltype(value)>;
        if constexpr (libk::SameAs<T, libk::monostate>) {
            KASSERT(false);
        } else if constexpr (libk::SameAs<T, Vproc*>) {
            kernel::sync::IrqLockGuard guard{value->state_lock_};
            KASSERT(value->execution_.scheduler_binding_ == &binding);
            value->execution_.scheduler_binding_ = nullptr;
        } else {
            kernel::sync::IrqLockGuard guard{value->stop_lock_};
            KASSERT(value->execution_.scheduler_binding_ == &binding);
            value->execution_.scheduler_binding_ = nullptr;
        }
    }, value_);
}

void Target::finish_stop() const noexcept {
    libk::visit([](auto value) noexcept {
        using T = libk::remove_cvr_t<decltype(value)>;
        if constexpr (libk::SameAs<T, libk::monostate>) {
            KASSERT(false);
        } else {
            KASSERT(value != nullptr);
            value->finish_stop();
        }
    }, value_);
}

TargetHold::operator bool() const noexcept {
    return !libk::holds_alternative<libk::monostate>(value_);
}

auto TargetHold::get() noexcept -> Target {
    return libk::visit([](auto& value) noexcept -> Target {
        using T = libk::remove_cvr_t<decltype(value)>;
        if constexpr (libk::SameAs<T, libk::monostate>) {
            return {};
        } else {
            return Target{value.get()};
        }
    }, value_);
}

auto TargetHold::get() const noexcept -> Target {
    return libk::visit([](const auto& value) noexcept -> Target {
        using T = libk::remove_cvr_t<decltype(value)>;
        if constexpr (libk::SameAs<T, libk::monostate>) {
            return {};
        } else if constexpr (libk::SameAs<T, object::ThreadHold>) {
            return Target{const_cast<Thread&>(value.get())};
        } else {
            return Target{const_cast<Vproc&>(value.get())};
        }
    }, value_);
}

auto TargetHold::reference() const noexcept
    -> libk::Expected<object::ObjectRef, object::ObjectError> {
    return libk::visit([](const auto& value) noexcept
        -> libk::Expected<object::ObjectRef, object::ObjectError> {
        using T = libk::remove_cvr_t<decltype(value)>;
        if constexpr (libk::SameAs<T, libk::monostate>) {
            return libk::unexpected(object::ObjectError::InvalidIdentity);
        } else {
            return value.ref();
        }
    }, value_);
}

} // namespace kernel::execution
