#include <sched/wake_queue.hpp>

#include <core/debug.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel::sched {

auto WakeQueue::post(Binding& binding) noexcept -> PostResult {
    kernel::sync::IrqLockGuard guard{lock_};
    if (binding.wake_pending_) {
        return PostResult{true};
    }
    binding.wake_pending_ = true;
    queue_.push_back(binding);
    delivery_.publish();
    return PostResult{true};
}

auto WakeQueue::claim_transport() noexcept
    -> libk::optional<kernel::IpiDelivery::Token> {
    kernel::sync::IrqLockGuard guard{lock_};
    return queue_.empty()
        ? libk::optional<kernel::IpiDelivery::Token>{}
        : delivery_.claim();
}

void WakeQueue::transport_failed(
    kernel::IpiDelivery::Token token) noexcept {
    kernel::sync::IrqLockGuard guard{lock_};
    if (queue_.empty()) {
        delivery_.consume();
    } else {
        delivery_.fail(token);
    }
}

auto WakeQueue::take() noexcept -> Binding* {
    kernel::sync::IrqLockGuard guard{lock_};
    if (queue_.empty()) {
        delivery_.consume();
        return nullptr;
    }
    Binding& binding = queue_.pop_front();
    if (queue_.empty()) {
        delivery_.consume();
    }
    return &binding;
}

void WakeQueue::complete(Binding& binding) noexcept {
    kernel::sync::IrqLockGuard guard{lock_};
    KASSERT(binding.wake_pending_);
    KASSERT(!binding.wake_hook_.is_linked());
    binding.wake_pending_ = false;
}

auto WakeQueue::size() const noexcept -> usize {
    kernel::sync::IrqLockGuard guard{lock_};
    return queue_.size();
}

} // namespace kernel::sched
