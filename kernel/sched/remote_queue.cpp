#include <optional>
#include <sched/remote_queue.hpp>
#include <trace.hpp>

namespace sched {

RemoteRequest::RemoteRequest(RemoteKind kind, void* owner) noexcept
    : owner_kind_(reinterpret_cast<usize>(owner) | static_cast<usize>(kind)) {
    libk_assert(owner && (reinterpret_cast<usize>(owner) & kind_mask) == 0);
}
RemoteRequest::~RemoteRequest() noexcept { libk_assert(!pending() && !hook_.is_linked()); }

auto RemoteQueue::post(RemoteRequest& request) noexcept -> RemotePost {
    sync::Lock guard{lock_};
    if (request.pending()) return RemotePost::Coalesced;
    request.pending_.store<libk::MemoryOrder::Release>(true);
    queue_.push_back(request);
    delivery_.publish();
    trace::emit(trace::Event::Post, reinterpret_cast<u64>(request.owner()),
                reinterpret_cast<u64>(&request), static_cast<u64>(request.kind()));
    return RemotePost::Inserted;
}

auto RemoteQueue::claim_transport() noexcept -> std::optional<IpiDelivery::Token> {
    sync::Lock guard{lock_};
    auto token = queue_.empty() ? std::optional<IpiDelivery::Token>{} : delivery_.claim();
    if (token) trace::emit(trace::Event::Kick, home_.raw, reinterpret_cast<u64>(this), token->generation);
    return token;
}

void RemoteQueue::transport_failed(IpiDelivery::Token token) noexcept {
    delivery_.fail(token);
    trace::emit(trace::Event::KickFail, home_.raw, reinterpret_cast<u64>(this), token.generation);
}

auto RemoteQueue::take() noexcept -> RemoteRequest* {
    sync::Lock guard{lock_};
    if (queue_.empty()) { delivery_.consume(); return nullptr; }
    auto& request = queue_.pop_front();
    if (queue_.empty()) delivery_.consume();
    trace::emit(trace::Event::Take, reinterpret_cast<u64>(request.owner()),
                reinterpret_cast<u64>(&request), static_cast<u64>(request.kind()));
    return &request;
}

void RemoteQueue::complete(RemoteRequest& request) noexcept {
    sync::Lock guard{lock_};
    libk_assert(request.pending() && !request.hook_.is_linked());
    trace::emit(trace::Event::Complete, reinterpret_cast<u64>(request.owner()),
                reinterpret_cast<u64>(&request), static_cast<u64>(request.kind()));
    request.pending_.store<libk::MemoryOrder::Release>(false);
}

auto RemoteQueue::cancel(RemoteRequest& request) noexcept -> RemoteCancel {
    sync::Lock guard{lock_};
    if (!request.pending()) return RemoteCancel::NotPending;
    if (!request.hook_.is_linked()) return RemoteCancel::AlreadyClaimed;
    queue_.erase(request);
    trace::emit(trace::Event::Cancel, reinterpret_cast<u64>(request.owner()),
                reinterpret_cast<u64>(&request), static_cast<u64>(request.kind()));
    request.pending_.store<libk::MemoryOrder::Release>(false);
    if (queue_.empty()) delivery_.consume();
    return RemoteCancel::CanceledQueued;
}

auto RemoteQueue::size() const noexcept -> usize {
    sync::Lock guard{lock_};
    return queue_.size();
}

} // namespace sched
