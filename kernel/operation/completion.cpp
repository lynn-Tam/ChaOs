#include <operation/completion.hpp>

#include <core/debug.hpp>
#include <cpu/cpu_registry.hpp>
#include <sched/binding.hpp>
#include <sched/dispatcher.hpp>
#include <thread/thread.hpp>
#include <execution/vproc.hpp>

namespace kernel::operation {

Completion::~Completion() noexcept {
    KASSERT(owner_ != nullptr && ops_ != nullptr);
    KASSERT(!attached());
}

void Completion::attach(Thread& thread, CpuRegistry& cpus) noexcept {
    KASSERT(!attached());
    KASSERT(thread.binding() != nullptr);
    sink_.template emplace<ThreadSink>(ThreadSink{&thread, &cpus});
    delivery_.store<libk::MemoryOrder::Release>(Delivery::Attached);
}

void Completion::attach(
    Vproc& vproc,
    CpuRegistry& cpus,
    operation::Key key) noexcept {
    KASSERT(!attached() && key.valid());
    KASSERT(vproc.binding() != nullptr);
    sink_.template emplace<VprocSink>(VprocSink{&vproc, &cpus, key});
    delivery_.store<libk::MemoryOrder::Release>(Delivery::Attached);
}

void Completion::signal() noexcept {
    KASSERT(complete());
    Delivery expected = Delivery::Attached;
    if (!delivery_.compare_exchange_strong<
            libk::MemoryOrder::AcqRel,
            libk::MemoryOrder::Acquire>(expected, Delivery::Claimed)) {
        KASSERT(expected == Delivery::Claimed
            || expected == Delivery::Ready
            || expected == Delivery::Detached);
        return;
    }
    if (auto* const thread = libk::get_if<ThreadSink>(&sink_)) {
        KASSERT(thread->thread != nullptr && thread->cpus != nullptr);
        sched::Binding* const binding = thread->thread->binding();
        KASSERT(binding != nullptr);
        KASSERT(sched::wake(*thread->cpus, *binding));
        delivery_.store<libk::MemoryOrder::Release>(Delivery::Ready);
        return;
    }
    const VprocSink target = *libk::get_if<VprocSink>(&sink_);
    KASSERT(target.vproc != nullptr && target.cpus != nullptr);
    const Result result = ops_->read(owner_);
    target.vproc->publish_operation(target.key, result, *target.cpus);
    sink_.template emplace<libk::monostate>();
    delivery_.store<libk::MemoryOrder::Release>(Delivery::Detached);
    ops_->release(owner_);
}

void Completion::finish(arch::TrapContext& trap) noexcept {
    KASSERT(complete());
    Delivery expected = Delivery::Ready;
    KASSERT((delivery_.compare_exchange_strong<
        libk::MemoryOrder::AcqRel,
        libk::MemoryOrder::Acquire>(expected, Delivery::Claimed)));
    const Result result = ops_->read(owner_);
    detach();
    trap.set_result(
        0, static_cast<usize>(static_cast<isize>(result.status)));
    trap.set_result(1, result.value);
    ops_->release(owner_);
}

auto Completion::cancel() noexcept -> bool {
    Delivery observed = delivery_.load<libk::MemoryOrder::Acquire>();
    for (;;) {
        if (observed == Delivery::Detached
            || observed == Delivery::Claimed) {
            return false;
        }
        KASSERT(observed == Delivery::Attached
            || observed == Delivery::Ready);
        if (delivery_.compare_exchange_weak<
                libk::MemoryOrder::AcqRel,
                libk::MemoryOrder::Acquire>(observed, Delivery::Claimed)) {
            break;
        }
    }

    bool drain = observed == Delivery::Ready || complete();
    if (!drain && !ops_->cancel(owner_)) {
        drain = complete();
        if (!drain) {
            delivery_.store<libk::MemoryOrder::Release>(Delivery::Attached);
            return false;
        }
    }
    if (drain) {
        static_cast<void>(ops_->read(owner_));
    }
    sink_.template emplace<libk::monostate>();
    delivery_.store<libk::MemoryOrder::Release>(Delivery::Detached);
    ops_->release(owner_);
    return true;
}

void Completion::detach() noexcept {
    KASSERT(delivery_.load<libk::MemoryOrder::Acquire>()
        == Delivery::Claimed);
    sink_.template emplace<libk::monostate>();
    delivery_.store<libk::MemoryOrder::Release>(Delivery::Detached);
}

} // namespace kernel::operation
