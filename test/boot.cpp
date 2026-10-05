#include <test/boot.hpp>

#include <arch/boot_stack.hpp>
#include <arch/ipi.hpp>
#include <libk/assert.hpp>
#include <cpu/registry.hpp>
#include <cpu/runtime.hpp>
#include <test/scenario.hpp>
#include <test/test.hpp>

namespace test::scenario {
extern const Id selected = static_cast<Id>(TEST_SCENARIO);
}

namespace test {

void run(const BootInfo& boot, const mm::Pmm& memory) noexcept {
    const TestStats stats = run_builtin_tests(boot, memory);
    libk_assert(scenario::run(scenario::selected, boot));
    libk_assert(arch_boot_stack_guard_intact());
    libk_assert(stats.failed == 0);
}

void runtime(CpuRuntime& cpu) noexcept {
    if (cpu.owner_registry == nullptr || cpu.local.descriptor == nullptr
        || cpu.local.descriptor->logical_id()
            != cpu.owner_registry->boot_id()) {
        return;
    }
    libk_assert(scenario::run_runtime(scenario::selected, cpu));
#if TEST_PANIC
    //Confirmatory experiment. Keep the fault injection in the test executable.
#if TEST_PANIC == 2
    arch::inject_ipi_failures_for_test(MaxCpus);
#endif
    libk::assert_fail({"false", __FILE__, LIBK_ASSERT_FUNCTION, __LINE__});
#endif
}

} // namespace test
