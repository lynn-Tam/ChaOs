#include <test/boot.hpp>
#include <boot/start.hpp>

#include <boot/info.hpp>
#include <cpu.hpp>
#include <cpu/ipi.hpp>
#include <libk/assert.hpp>
#include <cpu/cpu.hpp>
#include <test/scenario.hpp>
#include <test/test.hpp>
#if TEST_SCENARIO == 12
#include <console.hpp>
#include <sched/dispatcher.hpp>
#include <task/thread.hpp>
#endif

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
#if TEST_SCENARIO == 12
// Physical pressure fixture: owns real PMM pages without reducing the already
// granted client Quotas. Production entry and UAPI contain no injection hooks.
static libk::manual_lifetime<std::array<mm::OwnedPage, 32768>> ballast_storage;
static libk::manual_lifetime<object::ref<Thread>> pressure_thread;
static libk::manual_lifetime<object::ref<sched::Sc>> pressure_context;
static void pressure_entry(void*) noexcept {
    auto& ballast = *ballast_storage;
    auto& cpu = current_cpu();
    const auto pause = [&](u64 seconds) {
        sync::Irq interrupts;
        Source timer;
        auto& thread = *cpu.dispatcher().current();
        libk_assert(timer.await({thread, *cpu.cpus, cpu.dispatcher()}, {},
            boot->clock.rd_ticks() + seconds * boot->clock.rd_freq()).status == STATUS_PENDING);
        thread.block();
        libk_assert(thread.wait().result().status == STATUS_TIMED_OUT);
    };
    pause(10);
    usize held{};
    while (boot->pmm.free_page_count() > 1024 && held < ballast.size()) {
        auto page = boot->pmm.allocate_page(); libk_assert(page);
        ballast[held++] = std::move(*page);
        if (held % 64 == 0) sched::yield();
    }
    console::print<"[pressure] held={} free={}\n">(held * mm::page_size, boot->pmm.free_page_count() * mm::page_size);
    pause(120);
    for (auto& page : ballast) page.reset();
    console::print<"[pressure] released free={}\n">(boot->pmm.free_page_count() * mm::page_size);
}
#endif
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
#if TEST_SCENARIO == 12
    auto stack = mm::Stack::create(boot->vm); libk_assert(stack);
    auto thread = boot->objects.get<Thread>().create(std::move(*stack), Env::kernel(boot->vm),
        Thread::KernelStart{pressure_entry, nullptr});
    libk_assert(thread); (void)ballast_storage.emplace();
    auto& running = pressure_thread.emplace(thread->publish());
    libk_assert(running->claim_home(cpu.dispatcher()));
    auto sc = boot->objects.get<sched::Sc>().create(sched::Sc::Config{}, boot->clock.rd_ticks());
    libk_assert(sc); auto& context = pressure_context.emplace(sc->publish());
    libk_assert(boot->domain->admit(context.get(), cpu.id));
    libk_assert(context->bind(context, running.get()));
    libk_assert(running->execution(true));
#else
    libk_assert(scenario::run_runtime(scenario::selected, cpu));
#endif
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
