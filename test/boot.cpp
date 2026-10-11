#include <test/boot.hpp>
#include <boot/start.hpp>

#include <boot/info.hpp>
#include <cpu.hpp>
#include <cpu/ipi.hpp>
#include <libk/assert.hpp>
#include <cpu/cpu.hpp>
#include <test/scenario.hpp>
#include <test/test.hpp>

// Test artifacts own their transport faults; the CPU port has no test state.
static std::atomic<usize> ipi_failures{};
bool send_ipi(CpuHwId target) noexcept {
    usize n = ipi_failures.load(std::memory_order_acquire);
    while (n) {
        if (ipi_failures.compare_exchange_weak(n, n - 1, std::memory_order_acq_rel, std::memory_order_acquire)) return false;
    }
    return static_cast<bool>(arch::send_ipi(target.raw));
}

namespace test::scenario {
extern const Id selected = static_cast<Id>(TEST_SCENARIO);
}

namespace test {
Boot* boot{};
void fail_ipis(usize n) noexcept {
    ipi_failures.store(n, std::memory_order_release);
}


void run(const BootInfo& boot, const mm::Pmm& memory) noexcept {
    const TestStats stats = run_builtin_tests(boot, memory);
    libk_assert(scenario::run(scenario::selected, boot));
    libk_assert(stats.failed == 0);
}

void runtime(Cpu& cpu) noexcept {
    if (cpu.cpus == nullptr
        || cpu.id
            != cpu.cpus->boot_id()) {
        return;
    }
    libk_assert(scenario::run_runtime(scenario::selected, cpu));
#if TEST_PANIC
    // The probe expects peer snapshots; readiness belongs to this fixture.
    const auto& clock = cpu.dispatcher().clock();
    const auto start = clock.rd_ticks();
    for (usize i = 0; i < cpu.cpus->count(); ++i) {
        const auto& peer = *cpu.cpus->get(CpuId{i});
        while (!peer.online() && clock.rd_ticks() - start < clock.rd_freq())
            asm volatile("" ::: "memory");
        libk_assert(peer.online());
    }
    //Confirmatory experiment. Keep the fault injection in the test executable.
#if TEST_PANIC == 2
    fail_ipis(MaxCpus);
#endif
    libk::assert_fail({"false", __FILE__, LIBK_ASSERT_FUNCTION, __LINE__});
#endif
}

} // namespace test
