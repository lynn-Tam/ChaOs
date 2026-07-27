#include <fault/terminal.hpp>

#include <libk/limits.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel::fault {

auto TerminalRecord::state() const noexcept -> State {
    kernel::sync::IrqLockGuard guard{lock_};
    return state_;
}

auto TerminalRecord::snapshot() const noexcept -> Snapshot {
    kernel::sync::IrqLockGuard guard{lock_};
    return value_;
}

auto TerminalRecord::claim(
    Reason reason,
    myos_status_t status,
    usize detail,
    usize pc,
    usize address,
    u8 locus) noexcept -> bool {
    Observer* notify[observer_capacity]{};
    usize notify_count{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != State::Live) {
            return false;
        }
        state_ = State::Exiting;
        if (value_.sequence == libk::numeric_limits<u64>::max()) {
            // Sequence exhaustion is terminal and deliberately does not wrap.
            value_ = Snapshot{
                .sequence = value_.sequence,
                .reason = Reason::InvariantFailure,
                .status = status,
                .detail = detail,
                .pc = pc,
                .address = address,
                .locus = locus,
            };
        } else {
            value_ = Snapshot{
                .sequence = value_.sequence + 1,
                .reason = reason,
                .status = status,
                .detail = detail,
                .pc = pc,
                .address = address,
                .locus = locus,
            };
        }
        state_ = State::Published;
        for (usize index = 0; index < observer_capacity; ++index) {
            if (observers_[index] != nullptr) {
                notify[notify_count++] = observers_[index];
            }
        }
    }
    for (usize index = 0; index < notify_count; ++index) {
        Observer* const observer = notify[index];
        if (observer->notify_ != nullptr) {
            observer->notify_(observer->context_);
        }
    }
    return true;
}

auto TerminalRecord::published() const noexcept -> bool {
    return state() == State::Published;
}

auto TerminalRecord::attach(Observer& observer) noexcept -> bool {
    kernel::sync::IrqLockGuard guard{lock_};
    if (observer.owner_ != nullptr || observer.notify_ == nullptr) {
        return false;
    }
    Observer** free = nullptr;
    for (Observer*& candidate : observers_) {
        if (candidate == nullptr) {
            free = &candidate;
            break;
        }
    }
    if (free == nullptr) {
        return false;
    }
    observer.owner_ = this;
    *free = &observer;
    return true;
}

void TerminalRecord::detach(Observer& observer) noexcept {
    kernel::sync::IrqLockGuard guard{lock_};
    if (observer.owner_ != this) {
        return;
    }
    for (Observer*& candidate : observers_) {
        if (candidate == &observer) {
            candidate = nullptr;
            break;
        }
    }
    observer.owner_ = nullptr;
}

} // namespace kernel::fault
