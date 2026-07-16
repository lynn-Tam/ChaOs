#include <arch/context.hpp>

#include <core/debug.hpp>

extern "C" void arch_riscv64_switch_context(
    arch::KernelContext* outgoing,
    const arch::KernelContext* incoming) noexcept;
extern "C" [[noreturn]] void arch_riscv64_enter_context(
    const arch::KernelContext* initial) noexcept;
extern "C" [[noreturn]] void arch_riscv64_context_start() noexcept;

namespace arch {

namespace {

void assert_restorable(const KernelContext& context) noexcept {
    KASSERT(context.ra != 0);
    KASSERT(context.sp != 0);
    KASSERT((context.sp & 0xfU) == 0);
}

} // namespace

void prepare_context(
    KernelContext& context,
    ContextStart start) noexcept {
    KASSERT(start.stack_top != 0);
    KASSERT((start.stack_top & 0xfU) == 0);
    KASSERT(start.entry != nullptr);

    context = {};
    context.ra = reinterpret_cast<usize>(&arch_riscv64_context_start);
    context.sp = start.stack_top;
    context.s0 = reinterpret_cast<usize>(start.entry);
    context.s1 = reinterpret_cast<usize>(start.argument);
}

void switch_context(
    KernelContext& outgoing,
    const KernelContext& incoming) noexcept {
    assert_restorable(incoming);
    arch_riscv64_switch_context(&outgoing, &incoming);
}

[[noreturn]] void enter_context(
    const KernelContext& initial) noexcept {
    assert_restorable(initial);
    arch_riscv64_enter_context(&initial);
}

} // namespace arch
