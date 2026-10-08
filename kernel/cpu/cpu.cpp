#include <cpu/cpu.hpp>
#include <cpu/ipi.hpp>
#include <object/pool.hpp>
#include <boot/info.hpp>
#include <trap.hpp>
#include <console.hpp>
#if TEST_ENABLED
#include <test/boot.hpp>
#endif
#include <panic.hpp>
#include <trace.hpp>
#include <memory>
#include <libk/scope_guard.hpp>

[[noreturn]] static void secondary(void*, usize) noexcept;

[[noreturn]] static void idle_entry(void* arg) noexcept {
    auto& cpu = *static_cast<Cpu*>(arg);
    const auto n = cpu.enter();
    // Each hart reports completion after its own release. No startup spin or
    // snapshot state is needed; the last entrant observes all online CPUs.
    if (n == cpu.cpus->count()) console::print<"cpu: online={}\n">(n);
    if (cpu.id == Cpus::boot_id()) {
#if TEST_ENABLED
        test::runtime(cpu);
#endif
        console::print<"runtime: entered\n">();
    }
    sched::yield();
    for (;;) arch::wait_for_interrupt();
}


Cpu::~Cpu() noexcept {
    dispatcher_storage.reset();
    if (idle_thread) {
        libk_assert(idle_thread.retire());
        idle_thread.reset();
    }
    if (log) std::destroy_at(log);
    if (panic) std::destroy_at(panic);
}

auto Cpu::prepare(mm::KSpace& vm, time::Clock& clock, mm::Stack* boot_stack) noexcept -> bool {
    auto& pmm = objects.pmm();
    if (!boot_stack) {
        auto stack = mm::Stack::create(vm);
        if (!stack) return false;
        init_stack.emplace(std::move(*stack));
    }
    auto emergency = mm::Stack::create(vm);
    if (!emergency) return false;
    emergency_stack.emplace(std::move(*emergency));

    static_assert(sizeof(PanicSlot) <= mm::page_size && sizeof(trace::Ring) <= mm::page_size);
    auto page = pmm.allocate_page();
    if (!page) return false;
    panic_page = std::move(*page);
    panic = std::construct_at(reinterpret_cast<PanicSlot*>(panic_page.bytes()));
    panic->cpu = id;
    panic->hardware = hw;
    panic->registry = cpus;
    if (trace::enabled()) {
        auto history = pmm.allocate_page();
        if (!history) return false;
        trace_page = std::move(*history);
        log = std::construct_at(reinterpret_cast<trace::Ring*>(trace_page.bytes()));
    }
    auto stack = mm::Stack::create(vm);
    if (!stack) return false;
    auto idle = objects.get<Thread>().create(std::move(*stack), Env::kernel(vm),
        Thread::KernelStart{idle_entry, this}, Thread::Kind::Idle);
    if (!idle) return false;
    idle_thread = std::move(*idle).publish();
    entry = {.owner = this, .emergency_stack = emergency_stack->top(), .panic = panic};
    dispatcher_storage.emplace(*this, this->idle(), clock);
    const auto top = boot_stack ? boot_stack->top() : init_stack->top();
    libk_assert(top && !(top & 15U));
    start = {.hart = hw.raw, .root = vm.cpu_root(), .stack = top,
             .arg = this, .entry = secondary};
    // No fallible operation may follow transfer of the currently executing stack.
    if (boot_stack) init_stack.emplace(std::move(*boot_stack));
    return true;
}

auto Cpu::enter() noexcept -> usize {
    libk_assert(current == &idle() && idle().state() == Thread::State::Running);
    libk_assert(entry.stack == idle().home_stack_top() && !online());
    // Pair with request_stop: a late entrant must observe the stop, or the
    // requester must observe online and send an IPI.
    online_.store<libk::MemoryOrder::SeqCst>(true);
    if (__atomic_load_n(&entry.stop, __ATOMIC_SEQ_CST))
        ::panic("CPU stopped during startup");
    return cpus->online_.fetch_add<libk::MemoryOrder::AcqRel>(1) + 1;
}

void Cpu::request_stop() noexcept {
    __atomic_store_n(&entry.stop, usize{1}, __ATOMIC_SEQ_CST);
    if (online_.load<libk::MemoryOrder::SeqCst>()) (void)send_ipi(hw);
}

Cpus::~Cpus() noexcept {
    for (auto* cpu : cpus_) std::destroy_at(cpu);
}

auto Cpus::add(CpuHwId hw, object::Objects& objects, cap::Graph& grants,
               mm::KSpace& vm, time::Clock& clock, mm::Stack* boot_stack) noexcept -> bool {
    if (cpus_.size() == MaxCpus) return false;
    auto page = pages_.owner().allocate_page();
    if (!page) return false;
    auto* cpu = std::construct_at(reinterpret_cast<Cpu*>(page->bytes()),
                                 CpuId{cpus_.size()}, hw, *this, objects, grants);
    libk::scope_exit rollback{[&]() noexcept { std::destroy_at(cpu); }};
    if (!cpu->prepare(vm, clock, boot_stack)) return false;
    libk_assert(pages_.attach(std::move(*page)));
    libk_assert(cpus_.try_push_back(cpu));
    (void)rollback.release();
    return true;
}

void Cpu::install(usize hart) noexcept {
    libk_assert(hw.raw == hart && !arch::interrupts_enabled());
    arch::install(entry);
    libk_assert(arch::local() == &entry && arch::install_trap());
    idle().env().kernel_vspace()->root().adopt(*this);
    if (ext_irq) arch::enable_ext_irq();
}

void Cpus::start() noexcept {
    for (usize i = 1; i < count(); ++i) {
        auto& cpu = *get(CpuId{i});
        // Publish after the entire CPU list and shared kernel state are ready.
        __atomic_store_n(&cpu.start.ready, CPU_START_READY, __ATOMIC_RELEASE);
        const auto pa = pages_.owner().phys(mm::Virt{reinterpret_cast<usize>(&cpu.start)}, sizeof(cpu.start));
        libk_assert(pa);
        const auto started = arch::start_secondary(cpu.hw, boot_layout.secondary.pa, pa->raw());
        if (!started)
            console::print<"cpu: start hart={} error={}\n">(cpu.hw.raw, started.error());
    }
}

[[noreturn]] static void secondary(void* arg, usize hart) noexcept {
    auto* cpu = static_cast<Cpu*>(arg);
    libk_assert(cpu);
    cpu->install(hart);
    cpu->dispatcher().enter_idle();
}

auto current_cpu() noexcept -> Cpu& {
    auto* const entry = arch::local();
    libk_assert(entry && entry->owner);
    return *entry->owner;
}
