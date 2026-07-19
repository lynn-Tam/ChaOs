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
        completion.attach(*this);
    }
    return true;
}

void Wait::finish(arch::TrapContext& trap) noexcept {
    Completion* completion{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        completion = completion_;
        KASSERT(completion != nullptr
            && ready_.load<libk::MemoryOrder::Acquire>());
        completion_ = nullptr;
        cpus_ = nullptr;
        binding_ = nullptr;
        ready_.store<libk::MemoryOrder::Relaxed>(false);
    }
    completion->finish(trap);
}

auto Wait::cancel() noexcept -> bool {
    Completion* completion{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        completion = completion_;
    }
    KASSERT(completion != nullptr);
    if (!completion->cancel()) {
        return false;
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

void Wait::wake() noexcept {
    ready_.store<libk::MemoryOrder::Release>(true);
    CpuRegistry* cpus{};
    sched::Binding* binding{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(completion_ != nullptr && cpus_ != nullptr);
        cpus = cpus_;
        binding = binding_;
        KASSERT(binding != nullptr);
    }
    KASSERT(binding != nullptr && sched::wake(*cpus, *binding));
}

} // namespace kernel::operation
