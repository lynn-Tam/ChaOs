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

RemoteRequest::RemoteRequest(RemoteKind kind, void* owner) noexcept
    : owner_kind_(
          reinterpret_cast<usize>(owner) | static_cast<usize>(kind)) {
    KASSERT(owner != nullptr);
    KASSERT(
        (reinterpret_cast<usize>(owner) & kind_mask) == 0);
}

RemoteRequest::~RemoteRequest() noexcept {
    KASSERT(!pending() && !hook_.is_linked());
    KASSERT(owner() != nullptr);
}

auto RemoteRequest::delivery() const noexcept
    -> diag::concurrency::ObservationKey {
    const u64 raw =
        pending_delivery_.load<libk::MemoryOrder::Acquire>();
    return raw > 1 ? diag::concurrency::ObservationKey{raw}
                   : diag::concurrency::ObservationKey{};
}

auto RemoteRequest::generation() const noexcept -> u64 {
    diag::concurrency::ObservationSnapshot snapshot{};
    auto observation =
        diag::concurrency::ObservationLease::borrow(delivery());
    return observation.snapshot(snapshot)
        ? snapshot.subject_generation : 0;
}

auto RemoteRequest::cause() const noexcept
    -> diag::concurrency::ObservationKey {
    diag::concurrency::ObservationSnapshot snapshot{};
    auto observation =
        diag::concurrency::ObservationLease::borrow(delivery());
    return observation.snapshot(snapshot)
        ? diag::concurrency::ObservationKey{snapshot.detail[0]}
        : diag::concurrency::ObservationKey{};
}

auto RemoteQueue::post(
    RemoteRequest& request,
    diag::concurrency::ObservationKey cause) noexcept -> RemotePostResult {
    bool inserted{};
    RemotePostResult result{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (request.pending()) {
            const auto retained_cause = request.cause();
            if (cause && retained_cause && cause != retained_cause) {
                result = {RemotePost::CauseConflict, request.delivery()};
                publish_summary();
            } else {
                if (cause && !retained_cause) {
                    auto observation =
                        diag::concurrency::ObservationLease::borrow(
                            request.delivery());
                    observation.detail(0, cause.raw);
                    result.disposition = RemotePost::CauseAttached;
                } else {
                    result.disposition = RemotePost::Coalesced;
                }
                result.delivery = request.delivery();
                auto observation =
                    diag::concurrency::ObservationLease::borrow(
                        result.delivery);
                observation.touch();
                publish_summary();
            }
        } else {
            increment_sat(summary_.post_epoch);
            u64 generation =
                summary_.post_epoch.load<libk::MemoryOrder::Relaxed>();
            if (generation == 0) {
                increment_sat(summary_.post_epoch);
                generation =
                    summary_.post_epoch.load<libk::MemoryOrder::Relaxed>();
            }
            auto observation =
                diag::concurrency::ObservationLease::reserve_on(
                    home_,
                    diag::concurrency::RecordKind::RemoteDelivery,
                    reinterpret_cast<u64>(&request),
                    generation,
                    diag::concurrency::Expectation::InternalFinite);
            observation.detail(0, cause.raw);
            observation.detail(
                1, reinterpret_cast<u64>(request.owner()));
            observation.detail(2, home_.raw);
            const auto delivery = observation.detach_key();
            request.pending_delivery_.store<libk::MemoryOrder::Release>(
                delivery ? delivery.raw : 1);
            ++pending_count_;
            const u64 tick = now();
            queue_.push_back(request);
            publish(
                request,
                diag::concurrency::RemotePhase::Posted,
                diag::concurrency::WaitKind::RemoteRequest);
            delivery_.publish();
            publish(
                request,
                diag::concurrency::RemotePhase::NeedsKick,
                diag::concurrency::WaitKind::IpiDelivery,
                delivery_.generation());
            summary_.last_post.store<libk::MemoryOrder::Release>(tick);
            publish_summary();
            inserted = true;
            result = {RemotePost::Inserted, delivery};
        }
    }
    diag::concurrency::record(
        diag::concurrency::FlightDomain::Remote,
        inserted ? diag::concurrency::FlightEvent::RemotePost
                 : diag::concurrency::FlightEvent::RemoteCoalesced,
        reinterpret_cast<u64>(&request),
        reinterpret_cast<u64>(request.owner()),
        request_kind(request.kind()),
        request.generation(),
        cause.raw);
    return result;
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
            publish_queued(
                diag::concurrency::RemotePhase::InFlight,
                result->generation);
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
            publish_queued(
                retry ? diag::concurrency::RemotePhase::Retry
                      : diag::concurrency::RemotePhase::NeedsKick,
                token.generation);
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
            publish(
                request,
                diag::concurrency::RemotePhase::Taken,
                diag::concurrency::WaitKind::SchedulerWake,
                delivery_.generation());
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
            result->generation(),
            result->cause().raw);
    }
    return result;
}

void RemoteQueue::accepted(
    RemoteRequest& request,
    bool accepted_request) noexcept {
    kernel::sync::IrqLockGuard guard{lock_};
    KASSERT(request.pending() && !request.hook_.is_linked());
    if (accepted_request) {
        publish(
            request,
            diag::concurrency::RemotePhase::Accepted,
            diag::concurrency::WaitKind::SchedulerReady,
            delivery_.generation());
    } else {
        auto observation =
            diag::concurrency::ObservationLease::borrow(request.delivery());
        observation.touch();
    }
}

void RemoteQueue::complete(RemoteRequest& request) noexcept {
    u64 generation{};
    diag::concurrency::ObservationKey cause{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(request.pending() && !request.hook_.is_linked());
        generation = request.generation();
        cause = request.cause();
        publish(
            request,
            diag::concurrency::RemotePhase::Completed,
            diag::concurrency::WaitKind::None,
            delivery_.generation());
        auto observation =
            diag::concurrency::ObservationLease::borrow(request.delivery());
        request.pending_delivery_.store<libk::MemoryOrder::Release>(0);
        observation.finish(
            static_cast<u32>(diag::concurrency::RemotePhase::Completed),
            cause.raw);
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
        generation,
        cause.raw);
}

auto RemoteQueue::cancel(RemoteRequest& request) noexcept -> RemoteCancel {
    RemoteCancel result{};
    u64 generation{};
    diag::concurrency::ObservationKey cause{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (!request.pending()) {
            result = RemoteCancel::NotPending;
        } else if (!request.hook_.is_linked()) {
            result = RemoteCancel::AlreadyClaimed;
        } else {
            queue_.erase(request);
            generation = request.generation();
            cause = request.cause();
            publish(
                request,
                diag::concurrency::RemotePhase::Cancelled,
                diag::concurrency::WaitKind::None,
                delivery_.generation());
            auto observation =
                diag::concurrency::ObservationLease::borrow(
                    request.delivery());
            request.pending_delivery_.store<libk::MemoryOrder::Release>(0);
            observation.finish(
                static_cast<u32>(diag::concurrency::RemotePhase::Cancelled),
                cause.raw);
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
            diag::concurrency::FlightEvent::RemoteComplete,
            reinterpret_cast<u64>(&request),
            reinterpret_cast<u64>(request.owner()),
            request_kind(request.kind()),
            generation,
            cause.raw);
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

void RemoteQueue::publish(
    RemoteRequest& request,
    diag::concurrency::RemotePhase phase,
    diag::concurrency::WaitKind wait,
    u64 transport_generation) noexcept {
    auto observation =
        diag::concurrency::ObservationLease::borrow(request.delivery());
    observation.detail(3, transport_generation);
    observation.transition(
        static_cast<u32>(phase),
        (request.generation() << 8) | static_cast<u32>(phase),
        wait,
        diag::concurrency::NodeRef::cpu(home_));
}

void RemoteQueue::publish_queued(
    diag::concurrency::RemotePhase phase,
    u64 transport_generation) noexcept {
    usize published{};
    for (RemoteRequest& request : queue_) {
        if (published == diag::concurrency::graph_capacity) {
            // Requests beyond the diagnostic graph horizon retain their
            // previous transport-wait phase. The canonical queue state and
            // functional IPI delivery are unaffected.
            break;
        }
        publish(
            request,
            phase,
            diag::concurrency::WaitKind::IpiDelivery,
            transport_generation);
        ++published;
    }
}

} // namespace kernel::sched
