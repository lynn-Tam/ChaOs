#include <execution/stop.hpp>

#include <core/debug.hpp>
#include <thread/thread.hpp>
#include <execution/vproc.hpp>

namespace kernel::execution {

namespace {

constexpr u32 stop_started = 1;
constexpr u32 stop_finished = 2;

} // namespace

Stop::~Stop() noexcept {
    KASSERT(target_ == nullptr);
    KASSERT(!hook_.is_linked());
    KASSERT(!started_ || complete_);
}

void Stop::start(Thread& thread) noexcept {
    KASSERT(!started_ && !complete_);
    KASSERT(target_ == nullptr);
    started_ = true;
    target_ = &thread.execution();
    // The Stop object is the obligation identity. A target may have several
    // simultaneous stop requests, so target address alone is not unique.
    auto observation = diag::concurrency::ObservationLease::reserve(
        diag::concurrency::RecordKind::ExecutionStop,
        reinterpret_cast<u64>(this),
        1,
        diag::concurrency::Expectation::InternalFinite);
    observation.transition(
        stop_started,
        stop_started,
        diag::concurrency::WaitKind::SchedulerActivation,
        diag::concurrency::NodeRef::external(reinterpret_cast<u64>(&thread)));
    observation.watch(true);
    observation_ = observation.detach_key();
    thread.request_stop(*this);
}

void Stop::finish(Thread& thread) noexcept {
    KASSERT(started_ && !complete_
        && target_ == &thread.execution() && !hook_.is_linked());
    target_ = nullptr;
    complete_ = true;
    auto observation =
        diag::concurrency::ObservationLease::borrow(observation_);
    observation.finish(stop_finished);
    observation_ = {};
    const Notifier notify = notifier_;
    if (notify) {
        notify();
    }
}

void Stop::start(Vproc& vproc) noexcept {
    KASSERT(!started_ && !complete_);
    KASSERT(target_ == nullptr);
    started_ = true;
    target_ = &vproc.execution();
    auto observation = diag::concurrency::ObservationLease::reserve(
        diag::concurrency::RecordKind::ExecutionStop,
        reinterpret_cast<u64>(this),
        1,
        diag::concurrency::Expectation::InternalFinite);
    observation.transition(
        stop_started,
        stop_started,
        diag::concurrency::WaitKind::SchedulerActivation,
        diag::concurrency::NodeRef::external(reinterpret_cast<u64>(&vproc)));
    observation.watch(true);
    observation_ = observation.detach_key();
    vproc.request_stop(*this);
}

void Stop::finish(Vproc& vproc) noexcept {
    KASSERT(started_ && !complete_
        && target_ == &vproc.execution() && !hook_.is_linked());
    target_ = nullptr;
    complete_ = true;
    auto observation =
        diag::concurrency::ObservationLease::borrow(observation_);
    observation.finish(stop_finished);
    observation_ = {};
    const Notifier notify = notifier_;
    if (notify) {
        notify();
    }
}

} // namespace kernel::execution
