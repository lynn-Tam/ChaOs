// Selected-architecture kernel execution context contract.

#pragma once

#include <arch/backend/context.hpp>

namespace arch {

using KernelContext = backend::KernelContext;
using ContextEntry = void (*)(void*) noexcept;

struct ContextStart final {
    usize stack_top{};
    // The initial continuation owns its termination path and must not return.
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
