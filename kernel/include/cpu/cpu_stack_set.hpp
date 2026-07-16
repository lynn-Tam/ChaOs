#pragma once

#include <libk/manual_lifetime.hpp>
#include <libk/noncopyable.hpp>
#include <mm/kernel_stack.hpp>

namespace kernel {

struct CpuStackSet final : private libk::noncopyable_nonmovable {
    CpuStackSet() noexcept = default;
    ~CpuStackSet() noexcept { reset(); }

    auto reset() noexcept -> void {
        emergency.reset();
        irq.reset();
        init.reset();
    }

    libk::ManualLifetime<KernelStack> init{};
    libk::ManualLifetime<KernelStack> irq{};
    libk::ManualLifetime<KernelStack> emergency{};
};

} // namespace kernel

