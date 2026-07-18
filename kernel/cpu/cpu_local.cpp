#include <cpu/cpu_local.hpp>

#include <thread/thread.hpp>
#include <execution/execution.hpp>
#include <execution/vproc.hpp>
#include <sched/dispatcher.hpp>

#include <arch/cpu.hpp>
#include <core/debug.hpp>

namespace kernel {

auto CpuLocal::current_thread() noexcept -> Thread* {
    return dispatcher_ != nullptr ? dispatcher_->current().thread() : nullptr;
}

auto CpuLocal::current_thread() const noexcept -> const Thread* {
    return dispatcher_ != nullptr ? dispatcher_->current().thread() : nullptr;
}

auto CpuLocal::current_vproc() noexcept -> Vproc* {
    return dispatcher_ != nullptr ? dispatcher_->current().vproc() : nullptr;
}

auto CpuLocal::current_vproc() const noexcept -> const Vproc* {
    return dispatcher_ != nullptr ? dispatcher_->current().vproc() : nullptr;
}

auto CpuLocal::kernel_vspace() const noexcept -> kernel::mm::KernelVSpace* {
    return current_execution_ != nullptr
        ? current_execution_->binding().kernel_vspace()
        : nullptr;
}

auto CpuLocal::vspace() const noexcept -> kernel::mm::VSpace* {
    return current_execution_ != nullptr
        ? current_execution_->binding().vspace()
        : nullptr;
}

auto CpuLocal::cspace() const noexcept -> cap::CSpace* {
    return current_execution_ != nullptr
        ? current_execution_->binding().cspace()
        : nullptr;
}

auto current_cpu() noexcept -> CpuLocal& {
    void* const owner = arch::current_cpu_owner();
    KASSERT(owner != nullptr);
    return *static_cast<CpuLocal*>(owner);
}

} // namespace kernel
