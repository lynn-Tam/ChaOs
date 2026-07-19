#include <operation/completion.hpp>

#include <core/debug.hpp>
#include <cpu/cpu_registry.hpp>
#include <sched/binding.hpp>
#include <sched/dispatcher.hpp>
#include <execution/vproc.hpp>
#include <operation/wait.hpp>

namespace kernel::operation {

Completion::~Completion() noexcept {
    KASSERT(owner_ != nullptr && ops_ != nullptr);
    KASSERT(!attached());
    KASSERT(!signal_pending_.load<libk::MemoryOrder::Acquire>());
}

void Completion::attach(Wait& wait) noexcept {
    KASSERT(!attached());
    KASSERT(!signal_pending_.load<libk::MemoryOrder::Acquire>());
    KASSERT(wait.attached());
    sink_.template emplace<BlockingSink>(BlockingSink{&wait});
    delivery_.store<libk::MemoryOrder::Release>(Delivery::Attached);
}

void Completion::attach(
    Vproc& vproc,
    CpuRegistry& cpus,
    operation::Key key) noexcept {
    KASSERT(!attached() && key.valid());
    KASSERT(!signal_pending_.load<libk::MemoryOrder::Acquire>());
    KASSERT(vproc.binding() != nullptr);
    sink_.template emplace<VprocSink>(VprocSink{&vproc, &cpus, key});
    delivery_.store<libk::MemoryOrder::Release>(Delivery::Attached);
}

void Completion::signal() noexcept {
    Delivery expected = Delivery::Attached;
    if (!delivery_.compare_exchange_strong<
            libk::MemoryOrder::AcqRel,
            libk::MemoryOrder::Acquire>(expected, Delivery::Claimed)) {
        if (expected == Delivery::Offering) {
            signal_pending_.store<libk::MemoryOrder::Release>(true);
            return;
        }
        KASSERT(expected == Delivery::Claimed
            || expected == Delivery::Ready
            || expected == Delivery::Detached);
        return;
    }
    // Claim the delivery edge before touching the operation owner. A cancel
    // may otherwise detach and reclaim the owner after publication was
    // decided but before this producer reaches signal().
    KASSERT(complete());
    if (auto* const blocking = libk::get_if<BlockingSink>(&sink_)) {
        KASSERT(blocking->wait != nullptr);
        blocking->wait->wake();
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

auto Completion::offer() noexcept -> bool {
    Delivery expected = Delivery::Attached;
    if (!delivery_.compare_exchange_strong<
            libk::MemoryOrder::AcqRel,
            libk::MemoryOrder::Acquire>(expected, Delivery::Offering)) {
        return false;
    }
    auto* const target = libk::get_if<VprocSink>(&sink_);
    KASSERT(target != nullptr && target->vproc != nullptr
        && target->cpus != nullptr);
    const bool offered = target->vproc->offer_operation(
        target->key, *this, *target->cpus);
    delivery_.store<libk::MemoryOrder::Release>(Delivery::Attached);
    if (signal_pending_.exchange<libk::MemoryOrder::AcqRel>(false)) {
        signal();
    }
    return offered;
}

void Completion::finish(arch::TrapContext& trap) noexcept {
    KASSERT(complete());
    Delivery expected = Delivery::Ready;
    KASSERT((delivery_.compare_exchange_strong<
        libk::MemoryOrder::AcqRel,
        libk::MemoryOrder::Acquire>(expected, Delivery::Claimed)));
    detach();
    if (ops_->resume != nullptr) {
        ops_->resume(owner_, trap);
        ops_->release(owner_);
        return;
    }
    const Result result = ops_->read(owner_);
    trap.set_result(
        0, static_cast<usize>(static_cast<isize>(result.status)));
    trap.set_result(1, result.value);
    ops_->release(owner_);
}

auto Completion::cancel() noexcept -> bool {
    Delivery observed = delivery_.load<libk::MemoryOrder::Acquire>();
    for (;;) {
        if (observed == Delivery::Detached
            || observed == Delivery::Claimed
            || observed == Delivery::Offering) {
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
    KASSERT(!signal_pending_.load<libk::MemoryOrder::Acquire>());
    delivery_.store<libk::MemoryOrder::Release>(Delivery::Detached);
}

} // namespace kernel::operation
