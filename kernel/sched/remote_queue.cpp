#include <sched/remote_queue.hpp>

#include <arch/time.hpp>
#include <core/debug.hpp>
#include <diag/concurrency.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel::sched {
namespace {

[[nodiscard]] auto now() noexcept -> u64 {
    return arch::read_clock().ticks();
}

[[nodiscard]] constexpr auto request_kind(RemoteKind kind) noexcept -> u32 {
    return static_cast<u32>(kind);
}

void increment_sat(libk::Atomic<u64>& value) noexcept {
    u64 current = value.load<libk::MemoryOrder::Relaxed>();
    for (;;) {
        if (current == libk::numeric_limits<u64>::max()) {
            return;
        }
        if (value.compare_exchange_weak<
                libk::MemoryOrder::Relaxed,
                libk::MemoryOrder::Relaxed>(current, current + 1)) {
            return;
        }
    }
}

} // namespace

RemoteRequest::~RemoteRequest() noexcept {
    KASSERT(!pending() && !hook_.is_linked());
    KASSERT(owner_ != nullptr);
}

void RemoteQueue::post(RemoteRequest& request) noexcept {
    bool inserted{};
    u64 generation{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (request.pending()) {
            publish_summary();
        } else {
            request.pending_.store<libk::MemoryOrder::Release>(true);
            increment_sat(summary_.post_epoch);
            generation = summary_.post_epoch.load<libk::MemoryOrder::Relaxed>();
            ++pending_count_;
            const u64 tick = now();
            queue_.push_back(request);
            delivery_.publish();
            summary_.last_post.store<libk::MemoryOrder::Release>(tick);
            publish_summary();
            inserted = true;
        }
    }
    diag::concurrency::record(
        diag::concurrency::FlightDomain::Remote,
        inserted ? diag::concurrency::FlightEvent::RemotePost
                 : diag::concurrency::FlightEvent::RemoteCoalesced,
        reinterpret_cast<u64>(&request),
        reinterpret_cast<u64>(request.owner()),
        request_kind(request.kind()),
        generation);
}

auto RemoteQueue::claim_transport() noexcept
    -> libk::optional<kernel::IpiDelivery::Token> {
    libk::optional<kernel::IpiDelivery::Token> result{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        result = queue_.empty()
            ? libk::optional<kernel::IpiDelivery::Token>{}
            : delivery_.claim();
        if (result) {
            summary_.last_transport.store<libk::MemoryOrder::Release>(now());
        }
        publish_summary();
    }
    if (result) {
        diag::concurrency::record(
            diag::concurrency::FlightDomain::Remote,
            diag::concurrency::FlightEvent::RemoteTransportClaim,
            reinterpret_cast<u64>(this),
            result->generation);
    }
    return result;
}

void RemoteQueue::transport_failed(
    kernel::IpiDelivery::Token token) noexcept {
    bool retry{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (queue_.empty()) {
            delivery_.consume();
        } else {
            delivery_.fail(token);
            retry = delivery_.retry_needed();
        }
        summary_.last_transport.store<libk::MemoryOrder::Release>(now());
        publish_summary();
    }
    diag::concurrency::record(
        diag::concurrency::FlightDomain::Remote,
        retry ? diag::concurrency::FlightEvent::RemoteRetry
              : diag::concurrency::FlightEvent::RemoteTransportFailure,
        reinterpret_cast<u64>(this),
        token.generation);
}

auto RemoteQueue::take() noexcept -> RemoteRequest* {
    RemoteRequest* result{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (queue_.empty()) {
            delivery_.consume();
        } else {
            RemoteRequest& request = queue_.pop_front();
            result = &request;
            if (queue_.empty()) {
                delivery_.consume();
            }
            increment_sat(summary_.take_epoch);
            summary_.last_take.store<libk::MemoryOrder::Release>(now());
        }
        publish_summary();
    }
    if (result != nullptr) {
        diag::concurrency::record(
            diag::concurrency::FlightDomain::Remote,
            diag::concurrency::FlightEvent::RemoteTake,
            reinterpret_cast<u64>(result),
            reinterpret_cast<u64>(result->owner()),
            request_kind(result->kind()),
            summary_.post_epoch.load<libk::MemoryOrder::Acquire>());
    }
    return result;
}

void RemoteQueue::complete(RemoteRequest& request) noexcept {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(request.pending() && !request.hook_.is_linked());
        request.pending_.store<libk::MemoryOrder::Release>(false);
        KASSERT(pending_count_ != 0);
        --pending_count_;
        increment_sat(summary_.complete_epoch);
        publish_summary();
    }
    diag::concurrency::record(
        diag::concurrency::FlightDomain::Remote,
        diag::concurrency::FlightEvent::RemoteComplete,
        reinterpret_cast<u64>(&request),
        reinterpret_cast<u64>(request.owner()),
        request_kind(request.kind()),
        summary_.post_epoch.load<libk::MemoryOrder::Acquire>());
}

auto RemoteQueue::cancel(RemoteRequest& request) noexcept -> RemoteCancel {
    RemoteCancel result{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (!request.pending()) {
            result = RemoteCancel::NotPending;
        } else if (!request.hook_.is_linked()) {
            result = RemoteCancel::AlreadyClaimed;
        } else {
            queue_.erase(request);
            request.pending_.store<libk::MemoryOrder::Release>(false);
            KASSERT(pending_count_ != 0);
            --pending_count_;
            if (queue_.empty()) {
                delivery_.consume();
            }
            static_cast<void>(summary_.complete_epoch.fetch_add<
                libk::MemoryOrder::Relaxed>(1));
            result = RemoteCancel::CanceledQueued;
        }
        publish_summary();
    }
    if (result == RemoteCancel::CanceledQueued) {
        diag::concurrency::record(
            diag::concurrency::FlightDomain::Remote,
            diag::concurrency::FlightEvent::RemoteRetry,
            reinterpret_cast<u64>(&request),
            reinterpret_cast<u64>(request.owner()),
            request_kind(request.kind()),
            summary_.post_epoch.load<libk::MemoryOrder::Acquire>());
    }
    return result;
}

auto RemoteQueue::size() const noexcept -> usize {
    kernel::sync::IrqLockGuard guard{lock_};
    return queue_.size();
}

void RemoteQueue::publish_summary() noexcept {
    summary_.queue_count.store<libk::MemoryOrder::Release>(queue_.size());
    summary_.pending_count.store<libk::MemoryOrder::Release>(pending_count_);
    if (queue_.empty()) {
        summary_.oldest_kind.store<libk::MemoryOrder::Release>(0);
        summary_.oldest_owner.store<libk::MemoryOrder::Release>(0);
    } else {
        const RemoteRequest& oldest = queue_.front();
        summary_.oldest_kind.store<libk::MemoryOrder::Release>(
            request_kind(oldest.kind()));
        summary_.oldest_owner.store<libk::MemoryOrder::Release>(
            reinterpret_cast<u64>(oldest.owner()));
    }
    summary_.delivery_state.store<libk::MemoryOrder::Release>(
        static_cast<u32>(delivery_.state()));
    summary_.delivery_generation.store<libk::MemoryOrder::Release>(
        delivery_.generation());
}

} // namespace kernel::sched
