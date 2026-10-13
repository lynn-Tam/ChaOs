#include <utility>
#include <test/scenario.hpp>

#include <boot/info.hpp>
#include <cpu.hpp>
#include <boot/start.hpp>
#include <test/boot.hpp>
#include <cpu/cpu.hpp>
#include <trace.hpp>
#include <console.hpp>
#include <mm/kspace.hpp>
#include <sched/sc.hpp>
#include <sched/dispatcher.hpp>
#include <task/thread.hpp>

namespace test::scenario {

auto ordinary(const BootInfo& boot) noexcept -> bool {
    const bool result = !boot.cpus.empty()
        && boot.timebase_frequency != 0
        && boot.firmware;
    if (result) {
        console::print<"[scenario] ordinary ok\n">();
    }
    return result;
}

auto initrd(const BootInfo& boot) noexcept -> bool {
    const bool result = boot.module
        && boot.module->size != 0
        && boot.module->pages().valid();
    if (result) {
        console::print<"[scenario] initrd ok\n">();
    }
    return result;
}

auto trap(Cpu& runtime) noexcept -> bool {
    if (runtime.cpus == nullptr
       ) {
        return false;
    }
    Cpus& cpus = *runtime.cpus;
    const CpuId boot = runtime.id;
    Cpu* target{};
    for (usize offset = 1; offset < cpus.count(); ++offset) {
        const CpuId id{(boot.raw + offset) % cpus.count()};
        Cpu* const candidate = cpus.get(id);
        if (candidate != nullptr && candidate->online()
            && trace::snapshot(*candidate).last != 0) {
            target = candidate;
            break;
        }
    }
    if (target == nullptr || !arch::ipi_available()) {
        return false;
    }
    const u64 before = trace::snapshot(*target).last;
    if (!arch::send_ipi(target->hw.raw)) {
        return false;
    }

    bool entered{};
    bool exited{};
    for (u32 spin = 0; spin < (1U << 20); ++spin) {
        const auto log = trace::snapshot(*target);
        trace::Sample value{};
        for (u64 index = log.first; index < log.last; ++index) {
            if (!log.read(index, value)
                || value.seq < before) {
                continue;
            }
            entered = entered
                || value.kind == trace::Event::TrapEnter;
            exited = exited
                || value.kind == trace::Event::TrapExit;
        }
        if (entered && exited) {
            console::print<"[scenario] trap ok\n">();
            return true;
        }
        std::atomic_signal_fence(std::memory_order_seq_cst);
    }
    return false;
}

namespace {

struct DispatchState {
    std::array<usize, 3> order{};
    usize count{};
};
struct DispatchArg { DispatchState* state; usize id; };

void dispatch_entry(void* p) noexcept {
    auto& a = *static_cast<DispatchArg*>(p);
    sync::Irq irq;
    a.state->order[a.state->count++] = a.id;
}

} // namespace

auto dispatch(Cpu& runtime) noexcept -> bool {
    libk_assert(test::boot && runtime.cpus);
    auto& k = *test::boot;
    auto& cpus = *runtime.cpus;
    const auto cpu = runtime.id;
    const auto budget = k.clock.ticks<1, time::ms>();
    const auto period = k.clock.ticks<10, time::ms>();
    libk_assert(budget && period);
    DispatchState state;
    std::array<DispatchArg, 3> args{};
    std::array<object::ref<Thread>, 3> threads{};
    std::array<object::ref<sched::Sc>, 3> scs{};
    constexpr std::array<u8, 3> priority{2, 5, 5};
    for (usize i = 0; i < threads.size(); ++i) {
        args[i] = {&state, i};
        auto stack = mm::Stack::create(k.vm);
        libk_assert(stack);
        auto t = k.objects.get<Thread>().create(std::move(*stack), Env::kernel(k.vm),
                                       Thread::KernelStart{dispatch_entry, &args[i]});
        libk_assert(t);
        threads[i] = std::move(*t).publish();
        auto c = k.objects.get<sched::Sc>().create(sched::Sc::Config{
            .budget=*budget, .period=*period, .urgency=*sched::Urgency::make(priority[i])},
            k.clock.rd_ticks());
        libk_assert(c);
        scs[i] = std::move(*c).publish();
        auto ref = threads[i];
        libk_assert(ref && k.domain.get().admit(scs[i].get(), cpu)
                    && scs[i]->bind(std::move(ref)));
    }
    // Publish all candidates before dispatch so urgency and equal-priority
    // FIFO are tested through real context switches, not a queue fixture.
    const bool enabled = arch::disable_interrupts();
    for (auto& sc : scs) libk_assert(sched::start(cpus, sc.get()));
    sched::yield();
    arch::restore_interrupts(enabled);
    const auto duration = k.clock.ticks<100, time::ms>();
    const auto until = duration ? libk::checked_add(k.clock.rd_ticks(), *duration) : std::nullopt;
    libk_assert(until);
    for (auto& thread : threads) {
        while (!thread->stopped() && k.clock.rd_ticks() < *until) sched::yield();
        libk_assert(thread->stopped());
    }
    const bool ordered = state.count == 3 && state.order == std::array<usize, 3>{1, 2, 0};
    for (usize i = 0; i < scs.size(); ++i) {
        libk_assert(!scs[i]->bound() && k.domain.get().unadmit(scs[i].get()));
        libk_assert(scs[i].destroy() && threads[i].destroy());
        scs[i].reset();
        threads[i].reset();
    }

    if (ordered) console::print<"[scenario] dispatch ok\n">();
    return ordered;
}

} // namespace test::scenario
