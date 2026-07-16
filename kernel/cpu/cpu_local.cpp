#include <cpu/cpu_local.hpp>

#include <thread/thread.hpp>

#include <arch/cpu.hpp>
#include <core/debug.hpp>

namespace kernel {

auto CpuLocal::kernel_vspace() const noexcept -> kernel::mm::KernelVSpace* {
    return current_thread_ != nullptr
        ? current_thread_->execution().kernel_vspace()
        : nullptr;
}

auto CpuLocal::vspace() const noexcept -> kernel::mm::VSpace* {
    return current_thread_ != nullptr
        ? current_thread_->execution().vspace()
        : nullptr;
}

auto CpuLocal::cspace() const noexcept -> cap::CSpace* {
    return current_thread_ != nullptr
        ? current_thread_->execution().cspace()
        : nullptr;
}

auto current_cpu() noexcept -> CpuLocal& {
    void* const owner = arch::current_cpu_owner();
    KASSERT(owner != nullptr);
    return *static_cast<CpuLocal*>(owner);
}

} // namespace kernel
