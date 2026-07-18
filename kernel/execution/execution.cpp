#include <execution/execution.hpp>

#include <core/debug.hpp>
#include <mm/kernel_stack_layout.hpp>

namespace kernel {

Execution::Execution(
    kernel::resource::Charge&& stack_charge,
    KernelStack&& home_stack,
    ExecutionBinding&& binding) noexcept
    : stack_charge_(libk::move(stack_charge)),
      stack_(libk::move(home_stack)),
      binding_(libk::move(binding)) {
    KASSERT(!stack_charge_
        || stack_charge_.budget() == kernel::resource::Budget{
            .memory = kernel::mm::KernelStackLayout::StackBytes});
    KASSERT(stack_.base() != 0 && stack_.size() != 0);
    KASSERT((stack_.top() & 0xfU) == 0);
}

void Execution::prepare(
    arch::ContextEntry entry,
    void* owner,
    usize kernel_stack_top) noexcept {
    KASSERT(!prepared_ && entry != nullptr && owner != nullptr);
    KASSERT(kernel_stack_top >= stack_.base()
        && kernel_stack_top <= stack_.top()
        && (kernel_stack_top & 0xfU) == 0);
    arch::prepare_context(context_, {
        .stack_top = kernel_stack_top,
        .entry = entry,
        .argument = owner,
    });
    prepared_ = true;
}

} // namespace kernel
