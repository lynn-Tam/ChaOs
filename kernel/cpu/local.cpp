#include <cpu/local.hpp>

#include <task/thread.hpp>
#include <sched/dispatcher.hpp>

#include <arch/cpu.hpp>
#include <libk/assert.hpp>

auto CpuLocal::kernel_vspace() const noexcept -> mm::KSpace* {
    return current_ != nullptr
        ? current_->env().kernel_vspace()
        : nullptr;
}

auto CpuLocal::vspace() const noexcept -> mm::VSpace* {
    return current_ != nullptr
        ? current_->env().vspace()
        : nullptr;
}

auto CpuLocal::cspace() const noexcept -> cap::CSpace* {
    return current_ != nullptr
        ? current_->env().cspace()
        : nullptr;
}

auto current_cpu() noexcept -> CpuLocal& {
    void* const owner = arch::current_cpu_owner();
    libk_assert(owner != nullptr);
    return *static_cast<CpuLocal*>(owner);
}
