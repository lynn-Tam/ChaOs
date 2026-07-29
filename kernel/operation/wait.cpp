#include <operation/wait.hpp>

#include <core/debug.hpp>
#include <libk/utility.hpp>
#include <operation/completion.hpp>
#include <sched/dispatcher.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel::operation {

Wait::~Wait() noexcept {
    KASSERT(!attached());
}

auto Wait::attached() const noexcept -> bool {
    kernel::sync::IrqLockGuard guard{lock_};
    return completion_ != nullptr;
}

auto Wait::ready() const noexcept -> bool {
    return ready_.load<libk::MemoryOrder::Acquire>();
}

auto Wait::observation_key() const noexcept
    -> diag::concurrency::ObservationKey {
    kernel::sync::IrqLockGuard guard{lock_};
    return completion_ == nullptr
        ? diag::concurrency::ObservationKey{}
        : completion_->observation_key();
}

auto Wait::begin(
    Completion& completion,
    CpuRegistry& cpus,
    sched::Binding& binding) noexcept -> bool {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (completion_ != nullptr || completion.attached()) {
            return false;
        }
        completion_ = &completion;
        cpus_ = &cpus;
        binding_ = &binding;
        ready_.store<libk::MemoryOrder::Relaxed>(false);
        completion.attach(*this, binding);
        binding_->link_wait(
            completion.observation_key(),
            diag::concurrency::WaitKind::OperationCompletion,
            diag::concurrency::NodeRef::observation(
                completion.observation_key()));
    }
    return true;
}

void Wait::finish(arch::TrapContext& trap) noexcept {
    Completion* completion{};
    sched::Binding* binding{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        completion = completion_;
        binding = binding_;
        KASSERT(completion != nullptr
            && ready_.load<libk::MemoryOrder::Acquire>());
        completion_ = nullptr;
        cpus_ = nullptr;
        binding_ = nullptr;
        ready_.store<libk::MemoryOrder::Relaxed>(false);
    }
    if (binding != nullptr) {
        binding->clear_wait();
    }
    completion->finish(trap);
}

auto Wait::cancel() noexcept -> bool {
    Completion* completion{};
    sched::Binding* binding{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        completion = completion_;
        binding = binding_;
    }
    KASSERT(completion != nullptr);
    if (!completion->cancel()) {
        return false;
    }
    if (binding != nullptr) {
        binding->clear_wait();
    }
    {
        kernel::sync::IrqLockGuard guard{lock_};
        completion_ = nullptr;
        cpus_ = nullptr;
        binding_ = nullptr;
        ready_.store<libk::MemoryOrder::Relaxed>(false);
    }
    return true;
}

auto Wait::wake() noexcept -> diag::concurrency::ObservationKey {
    ready_.store<libk::MemoryOrder::Release>(true);
    CpuRegistry* cpus{};
    sched::Binding* binding{};
    Completion* completion{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(completion_ != nullptr && cpus_ != nullptr);
        completion = completion_;
        cpus = cpus_;
        binding = binding_;
        KASSERT(binding != nullptr);
    }
    diag::concurrency::ObservationKey delivery{};
    KASSERT(binding != nullptr && sched::wake(
        *cpus,
        *binding,
        completion->observation_key(),
        &delivery));
    return delivery;
}

} // namespace kernel::operation
