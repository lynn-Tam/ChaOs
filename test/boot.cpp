#include <test/boot.hpp>

#include <arch/boot_stack.hpp>
#include <core/debug.hpp>
#include <cpu/cpu_registry.hpp>
#include <cpu/cpu_runtime.hpp>
#include <test/scenario.hpp>
#include <test/test.hpp>

namespace kernel::test {

void run(const kernel::boot::BootInfo& boot) noexcept {
    const TestStats stats = run_builtin_tests(boot);
    KASSERT(scenario::run(diag::scenario::selected, boot));
    KASSERT(arch_boot_stack_guard_intact());
    KASSERT(stats.failed == 0);
}

void runtime(kernel::CpuRuntime& cpu) noexcept {
    if (cpu.owner_registry == nullptr || cpu.local.descriptor == nullptr
        || cpu.local.descriptor->logical_id()
            != cpu.owner_registry->boot_id()) {
        return;
    }
    KASSERT(scenario::run_runtime(diag::scenario::selected, cpu));
}

} // namespace kernel::test
