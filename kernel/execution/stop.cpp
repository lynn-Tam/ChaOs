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
    KASSERT(libk::holds_alternative<libk::monostate>(target_));
    KASSERT(!hook_.is_linked());
    KASSERT(!started_ || complete_);
}

void Stop::start(Thread& thread) noexcept {
    KASSERT(!started_ && !complete_);
    KASSERT(libk::holds_alternative<libk::monostate>(target_));
    started_ = true;
    target_.template emplace<Thread*>(&thread);
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
    static_cast<void>(observation.detach_key());
    thread.request_stop(*this);
}

void Stop::finish(Thread& thread) noexcept {
    auto** const target = libk::get_if<Thread*>(&target_);
    KASSERT(started_ && !complete_ && target != nullptr
        && *target == &thread && !hook_.is_linked());
    target_.template emplace<libk::monostate>();
    complete_ = true;
    auto observation = diag::concurrency::ObservationLease::find(
        diag::concurrency::RecordKind::ExecutionStop,
        reinterpret_cast<u64>(this),
        1);
    observation.finish(stop_finished);
    const Notifier notify = notifier_;
    if (notify) {
        notify();
    }
}

void Stop::start(Vproc& vproc) noexcept {
    KASSERT(!started_ && !complete_);
    KASSERT(libk::holds_alternative<libk::monostate>(target_));
    started_ = true;
    target_.template emplace<Vproc*>(&vproc);
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
    static_cast<void>(observation.detach_key());
    vproc.request_stop(*this);
}

void Stop::finish(Vproc& vproc) noexcept {
    auto** const target = libk::get_if<Vproc*>(&target_);
    KASSERT(started_ && !complete_ && target != nullptr
        && *target == &vproc && !hook_.is_linked());
    target_.template emplace<libk::monostate>();
    complete_ = true;
    auto observation = diag::concurrency::ObservationLease::find(
        diag::concurrency::RecordKind::ExecutionStop,
        reinterpret_cast<u64>(this),
        1);
    observation.finish(stop_finished);
    const Notifier notify = notifier_;
    if (notify) {
        notify();
    }
}

} // namespace kernel::execution
