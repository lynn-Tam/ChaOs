#include <sched/remote_queue.hpp>

#include <arch/cpu.hpp>
#include <arch/time.hpp>
#include <core/debug.hpp>
#include <diag/concurrency.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel::sched {
namespace {

constexpr u64 max_request_generation =
    libk::numeric_limits<u64>::max() >> 1;

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
    return diag::concurrency::ObservationKey{
        delivery_.load<libk::MemoryOrder::Acquire>()};
}

auto RemoteRequest::diagnostic_cause(
    diag::concurrency::ObservationKey delivery) const noexcept
    -> diag::concurrency::ObservationKey {
    auto observation =
        diag::concurrency::ObservationLease::borrow(delivery);
    diag::concurrency::ObservationSnapshot snapshot{};
    if (!observation.snapshot(snapshot)
        || snapshot.record_kind
            != diag::concurrency::RecordKind::RemoteDelivery
        || snapshot.subject_identity != reinterpret_cast<u64>(this)) {
        return {};
    }
    return diag::concurrency::ObservationKey{snapshot.detail[0]};
}

void RemoteRequest::retain_diagnostic_cause(
    diag::concurrency::ObservationKey delivery,
    diag::concurrency::ObservationKey cause) const noexcept {
    if (!cause) {
        return;
    }
    auto observation =
        diag::concurrency::ObservationLease::borrow(delivery);
    diag::concurrency::ObservationSnapshot snapshot{};
    if (!observation.snapshot(snapshot)
        || snapshot.record_kind
            != diag::concurrency::RecordKind::RemoteDelivery
        || snapshot.subject_identity != reinterpret_cast<u64>(this)
        || snapshot.detail[0] != 0) {
        return;
    }
    observation.detail(0, cause.raw);
}

auto RemoteRequest::generation() const noexcept -> u64 {
    return state_.load<libk::MemoryOrder::Acquire>() >> 1;
}

auto RemoteQueue::post_locked(
    RemoteRequest& request,
    diag::concurrency::ObservationKey cause,
    bool diagnostics,
    bool& inserted) noexcept -> RemotePostResult {
    RemotePostResult result{};
    if (request.pending()) {
        // Coalescing is a canonical pending-edge decision.  A wake cause is
        // only explanatory; retaining the first nonzero projection cannot
        // reject or alter the already retained request.  The best-effort
        // producer deliberately skips this diagnostic read/write path.
        const auto delivery = request.delivery();
        if (diagnostics) {
            request.retain_diagnostic_cause(delivery, cause);
        }
        result.disposition = RemotePost::Coalesced;
        result.delivery = delivery;
        if (diagnostics) {
            auto observation =
                diag::concurrency::ObservationLease::borrow(result.delivery);
            observation.touch();
        }
        return result;
    }

    // The IRQ-off try path has no retrying diagnostic counters.  The queue
    // projection below remains visible; the full post epoch is reserved for
    // the ordinary traced scheduler path.
    if (diagnostics) {
        increment_sat(summary_.post_epoch);
    }
    u64 generation = request.generation();
    if (generation != max_request_generation) {
        ++generation;
    }
    if (generation == 0) {
        generation = 1;
    }
    if (diagnostics) {
        auto observation =
            diag::concurrency::ObservationLease::reserve_on(
                home_,
                diag::concurrency::RecordKind::RemoteDelivery,
                reinterpret_cast<u64>(&request),
                generation,
                diag::concurrency::Expectation::InternalFinite);
        diag::concurrency::ObservationBatch initial{
            .phase = static_cast<u32>(
                diag::concurrency::RemotePhase::Posted),
            .semantic_stamp = (generation << 8)
                | static_cast<u32>(
                    diag::concurrency::RemotePhase::Posted),
            .wait = diag::concurrency::WaitKind::RemoteRequest,
            .driver = diag::concurrency::NodeRef::cpu(home_),
            .site = diag::concurrency::SourceSite::current(),
            .detail_mask = 0xfU,
            .update_progress = true};
        initial.detail[0] = cause.raw;
        initial.detail[1] = reinterpret_cast<u64>(request.owner());
        initial.detail[2] = home_.raw;
        initial.detail[3] = 0;
        observation.publish(initial);
        request.delivery_.store<libk::MemoryOrder::Release>(
            observation.detach_key().raw);
    } else {
        // A silent request still follows the canonical pending/queue
        // protocol.  It simply has no diagnostic lease for a producer that
        // is running from an IRQ-off watchdog callback.
        request.delivery_.store<libk::MemoryOrder::Release>(0);
    }
    request.state_.store<libk::MemoryOrder::Release>(
        (generation << 1) | 1U);
    ++pending_count_;
    queue_.push_back(request);
    delivery_.publish();
    if (diagnostics) {
        publish(
            request,
            diag::concurrency::RemotePhase::NeedsKick,
            diag::concurrency::WaitKind::IpiDelivery,
            delivery_.generation());
    }
    if (diagnostics) {
        summary_.last_post.store<libk::MemoryOrder::Release>(now());
    }
    inserted = true;
    result = {RemotePost::Inserted, request.delivery()};
    return result;
}

auto RemoteQueue::post(
    RemoteRequest& request,
    diag::concurrency::ObservationKey cause) noexcept -> RemotePostResult {
    bool inserted{};
    RemotePostResult result{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        result = post_locked(request, cause, true, inserted);
        publish_summary();
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

auto RemoteQueue::try_post(
    RemoteRequest& request,
    diag::concurrency::ObservationKey cause) noexcept
    -> libk::optional<RemoteTryPostResult> {
    // This is an observer-internal path. The caller already owns the IRQ-off
    // boundary; use one raw try-lock without lock graph/flight/profile work.
    KASSERT(!arch::interrupts_enabled());
    kernel::sync::LockAccess::ObserverTryLockToken guard{lock_};
    if (!guard.owns_lock()) {
        return libk::nullopt;
    }

    bool inserted{};
    RemoteTryPostResult result{
        post_locked(request, cause, false, inserted),
        {}};
    // Claim while holding the same lock that admitted the request.  This
    // closes the only gap in which a producer could leave NeedsKick behind a
    // concurrent consumer that is already draining the queue.
    result.transport = queue_.empty()
        ? libk::optional<kernel::IpiDelivery::Token>{}
        : delivery_.claim();
    publish_summary();
    return result;
}

auto RemoteQueue::try_transport_failed(
    kernel::IpiDelivery::Token token) noexcept -> bool {
    const bool retry = delivery_.fail_if_inflight(token);
    if (retry) {
        // The queue projection is intentionally partial here: the canonical
        // IpiDelivery state changed locklessly, while queue ownership itself
        // remains protected by lock_.  A later ordinary queue pass republishes
        // the complete summary and diagnostic phases.
        summary_.delivery_state.store<libk::MemoryOrder::Release>(
            static_cast<u32>(delivery_.state()));
        summary_.delivery_generation.store<libk::MemoryOrder::Release>(
            delivery_.generation());
    }
    return retry;
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
            result->generation());
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
        cause = {};
        publish(
            request,
            diag::concurrency::RemotePhase::Completed,
            diag::concurrency::WaitKind::None,
            delivery_.generation());
        auto observation =
            diag::concurrency::ObservationLease::borrow(request.delivery());
        request.state_.store<libk::MemoryOrder::Release>(generation << 1);
        observation.finish(
            static_cast<u32>(diag::concurrency::RemotePhase::Completed),
            cause.raw);
        request.delivery_.store<libk::MemoryOrder::Release>(0);
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
            cause = {};
            publish(
                request,
                diag::concurrency::RemotePhase::Cancelled,
                diag::concurrency::WaitKind::None,
                delivery_.generation());
            auto observation =
                diag::concurrency::ObservationLease::borrow(
                    request.delivery());
            request.state_.store<libk::MemoryOrder::Release>(generation << 1);
            observation.finish(
                static_cast<u32>(diag::concurrency::RemotePhase::Cancelled),
                cause.raw);
            request.delivery_.store<libk::MemoryOrder::Release>(0);
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
    diag::concurrency::ObservationBatch update{
        .phase = static_cast<u32>(phase),
        .semantic_stamp = (request.generation() << 8)
            | static_cast<u32>(phase),
        .wait = wait,
        .driver = diag::concurrency::NodeRef::cpu(home_),
        .site = diag::concurrency::SourceSite::current(),
        .detail_mask = 1U << 3,
        .update_progress = true};
    update.detail[3] = transport_generation;
    observation.publish(update);
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
