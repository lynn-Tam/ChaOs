#include <sched/remote_queue.hpp>

#include <core/debug.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel::sched {

RemoteRequest::~RemoteRequest() noexcept {
    KASSERT(!pending_ && !hook_.is_linked());
    KASSERT(owner_ != nullptr);
}

void RemoteQueue::post(RemoteRequest& request) noexcept {
    kernel::sync::IrqLockGuard guard{lock_};
    if (request.pending_) {
        return;
    }
    request.pending_ = true;
    queue_.push_back(request);
    delivery_.publish();
}

auto RemoteQueue::claim_transport() noexcept
    -> libk::optional<kernel::IpiDelivery::Token> {
    kernel::sync::IrqLockGuard guard{lock_};
    return queue_.empty()
        ? libk::optional<kernel::IpiDelivery::Token>{}
        : delivery_.claim();
}

void RemoteQueue::transport_failed(
    kernel::IpiDelivery::Token token) noexcept {
    kernel::sync::IrqLockGuard guard{lock_};
    if (queue_.empty()) {
        delivery_.consume();
    } else {
        delivery_.fail(token);
    }
}

auto RemoteQueue::take() noexcept -> RemoteRequest* {
    kernel::sync::IrqLockGuard guard{lock_};
    if (queue_.empty()) {
        delivery_.consume();
        return nullptr;
    }
    RemoteRequest& request = queue_.pop_front();
    if (queue_.empty()) {
        delivery_.consume();
    }
    return &request;
}

void RemoteQueue::complete(RemoteRequest& request) noexcept {
    kernel::sync::IrqLockGuard guard{lock_};
    KASSERT(request.pending_ && !request.hook_.is_linked());
    request.pending_ = false;
}

auto RemoteQueue::cancel(RemoteRequest& request) noexcept -> RemoteCancel {
    kernel::sync::IrqLockGuard guard{lock_};
    if (!request.pending_) {
        return RemoteCancel::NotPending;
    }
    if (!request.hook_.is_linked()) {
        return RemoteCancel::AlreadyClaimed;
    }
    queue_.erase(request);
    request.pending_ = false;
    if (queue_.empty()) {
        delivery_.consume();
    }
    return RemoteCancel::CanceledQueued;
}

auto RemoteQueue::size() const noexcept -> usize {
    kernel::sync::IrqLockGuard guard{lock_};
    return queue_.size();
}

} // namespace kernel::sched
