#include <execution/stop.hpp>

#include <core/debug.hpp>
#include <thread/thread.hpp>
#include <execution/vproc.hpp>

namespace kernel::execution {

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
    thread.request_stop(*this);
}

void Stop::finish(Thread& thread) noexcept {
    auto** const target = libk::get_if<Thread*>(&target_);
    KASSERT(started_ && !complete_ && target != nullptr
        && *target == &thread && !hook_.is_linked());
    target_.template emplace<libk::monostate>();
    complete_ = true;
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
    vproc.request_stop(*this);
}

void Stop::finish(Vproc& vproc) noexcept {
    auto** const target = libk::get_if<Vproc*>(&target_);
    KASSERT(started_ && !complete_ && target != nullptr
        && *target == &vproc && !hook_.is_linked());
    target_.template emplace<libk::monostate>();
    complete_ = true;
    const Notifier notify = notifier_;
    if (notify) {
        notify();
    }
}

} // namespace kernel::execution
