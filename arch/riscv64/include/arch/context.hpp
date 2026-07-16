#pragma once

#include "arch/riscv64/context/kernel_context.hpp"

#include <core/types.hpp>

namespace arch {

using KernelContext = riscv64::KernelContext;
using ContextEntry = void (*)(void*) noexcept;

struct ContextStart final {
    usize stack_top{};
    ContextEntry entry{};
    void* argument{};
};

void prepare_context(
    KernelContext& context,
    ContextStart start) noexcept;

// Returns only after outgoing is selected and restored by a later switch.
void switch_context(
    KernelContext& outgoing,
    const KernelContext& incoming) noexcept;

[[noreturn]] void enter_context(
    const KernelContext& initial) noexcept;

} // namespace arch
