#include <mm/memory_work.hpp>

#include <core/debug.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel::mm {

MemoryExecutor::~MemoryExecutor() noexcept {
    KASSERT(!notifier_);
    KASSERT(queue_.empty());
    KASSERT(runner_ == nullptr);
}

/*luna change: preserve already-queued and runner-racing submissions, reason: executor admission must not drop a sole operations pin on a concurrent kick*/
auto MemoryExecutor::submit(MemoryObject& object) noexcept -> bool {
    const auto key = diag::concurrency::ObservationKey{
        object.observation_key_.load<libk::MemoryOrder::Acquire>()};
    auto observation = diag::concurrency::ObservationLease::borrow(key);
    Notifier notifier{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (!object.work_open_.load<libk::MemoryOrder::Acquire>()
            || !object.work_active_.load<libk::MemoryOrder::Acquire>()) {
            return false;
        }
        if (object.work_hook_.is_linked()) {
            return true;
        }
        queue_.push_back(object);
        notifier = notifier_;
    }
    if (observation) {
        observation.attempt(
            static_cast<u32>(diag::concurrency::ServicePhase::Queued),
            diag::concurrency::WaitKind::MemoryWork,
            {});
    }
    if (notifier) {
        const auto delivery = notifier();
        if (observation) {
            observation.attempt(
                static_cast<u32>(diag::concurrency::ServicePhase::WakeIssued),
                diag::concurrency::WaitKind::MemoryWork,
                delivery
                    ? diag::concurrency::NodeRef::observation(delivery)
                    : diag::concurrency::NodeRef{});
        }
    }
    return true;
}

auto MemoryExecutor::take() noexcept -> MemoryObject* {
    kernel::sync::IrqLockGuard guard{lock_};
    if (queue_.empty()) {
        return nullptr;
    }
    KASSERT(runner_ == nullptr);
    runner_ = &queue_.pop_front();
    return runner_;
}

/*luna change: withdraw only releases non-runner queue ownership, reason: a current runner still owns the MemoryObject operations pin until completion*/
auto MemoryExecutor::withdraw(MemoryObject& object) noexcept -> bool {
    kernel::sync::IrqLockGuard guard{lock_};
    if (object.work_hook_.is_linked()) {
        queue_.erase(object);
        return runner_ != &object;
    }
    return false;
}

auto MemoryExecutor::run(usize budget) noexcept -> MemoryServiceBatch {
    KASSERT(budget != 0);
    usize processed{};
    usize progressed{};
    for (; processed < budget; ++processed) {
        MemoryObject* const object = take();
        if (object == nullptr) {
            break;
        }
        const MemoryServiceBatch batch = object->service(1);
        progressed += batch.progressed;
        /*luna change: complete runner handoff before projecting service state, reason: a racing kick must be visible as queued work instead of a transient diagnostic idle*/
        bool settle{};
        bool queued{};
        {
            kernel::sync::IrqLockGuard guard{lock_};
            KASSERT(runner_ == object);
            if (object->work_hook_.is_linked()) {
                runner_ = nullptr;
                queued = true;
            } else if (batch.more
                && object->work_open_.load<libk::MemoryOrder::Acquire>()
                && object->work_active_.load<libk::MemoryOrder::Acquire>()) {
                queue_.push_back(*object);
                runner_ = nullptr;
                queued = true;
            } else {
                runner_ = nullptr;
                settle = true;
            }
        }
        MemoryServiceBatch observation = batch;
        observation.more = queued;
        object->publish_work_observation(observation);
        if (settle) {
            object->finish_work();
        }
        /*luna change: remove the write-only executor epoch update, reason: service progress has no MemoryExecutor epoch consumer*/
    }
    return MemoryServiceBatch{
        .processed = processed,
        .progressed = progressed,
        .more = pending(),
    };
}

auto MemoryExecutor::pending() const noexcept -> bool {
    kernel::sync::IrqLockGuard guard{lock_};
    return !queue_.empty();
}

void MemoryExecutor::bind_notifier(Notifier notifier) noexcept {
    KASSERT(notifier);
    kernel::sync::IrqLockGuard guard{lock_};
    KASSERT(!notifier_);
    notifier_ = notifier;
}

void MemoryExecutor::unbind_notifier() noexcept {
    kernel::sync::IrqLockGuard guard{lock_};
    notifier_.reset();
}

} // namespace kernel::mm
