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
        return value->wait_ != nullptr;
    }
    Vproc* const value = vproc();
    return value != nullptr && value->pending_operations();
}

auto Target::stop_requested() const noexcept -> bool {
    return libk::visit([](auto value) noexcept -> bool {
        using T = libk::remove_cvr_t<decltype(value)>;
        if constexpr (libk::SameAs<T, libk::monostate>) {
            return true;
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
        } else {
            kernel::sync::IrqLockGuard guard{value->stop_lock_};
            if constexpr (libk::SameAs<T, Thread*>) {
                return value->stop_requested_ && value->wait_ == nullptr;
            } else {
                return value->stop_requested_
                    && !value->pending_operations();
            }
        }
    }, value_);
}

auto Target::claim_home(sched::CpuDispatcher& home) const noexcept -> bool {
    return libk::visit([&home](auto value) noexcept -> bool {
        using T = libk::remove_cvr_t<decltype(value)>;
        if constexpr (libk::SameAs<T, libk::monostate>) {
            return false;
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

} // namespace kernel::execution
