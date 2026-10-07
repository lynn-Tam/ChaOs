#include <utility>
#include <test/scenario.hpp>

#include <boot/link.hpp>
#include <cpu.hpp>
#include <state.hpp>
#include <cpu/runtime.hpp>
#include <cpu/registry.hpp>
#include <trace.hpp>
#include <console.hpp>
#include <mm/kspace.hpp>
#include <sched/sc.hpp>
#include <sched/dispatcher.hpp>
#include <task/thread.hpp>

namespace test::scenario {

auto ordinary(const BootInfo& boot) noexcept -> bool {
    const bool result = boot.cpu
        && boot.cpu.summary().count != 0
        && boot.timebase_frequency != 0
        && boot.fdt
        && boot_guard_ok();
    if (result) {
        console::print<"[scenario] ordinary ok\n">();
    }
    return result;
}

auto initrd(const BootInfo& boot) noexcept -> bool {
    const bool result = boot.module
        && boot.module->size != 0
        && boot.module->pages.valid();
    if (result) {
        console::print<"[scenario] initrd ok\n">();
    }
    return result;
}

auto trap(CpuRuntime& runtime) noexcept -> bool {
    if (runtime.owner_registry == nullptr
        || runtime.local.descriptor == nullptr) {
        return false;
    }
    CpuRegistry& cpus = *runtime.owner_registry;
    const CpuId boot = runtime.local.descriptor->logical_id();
    CpuRuntime* target{};
    for (usize offset = 1; offset < cpus.count(); ++offset) {
        const CpuId id{(boot.raw + offset) % cpus.count()};
        const CpuDescriptor* const descriptor = cpus.descriptor(id);
        CpuRuntime* const candidate = cpus.runtime(id);
        if (descriptor != nullptr && candidate != nullptr
            && descriptor->state() == CpuState::Online
            && trace::snapshot(*candidate).last != 0) {
            target = candidate;
            break;
        }
    }
    if (target == nullptr || !arch::ipi_available()) {
        return false;
    }
    const u64 before = trace::snapshot(*target).last;
    if (!arch::send_ipi(target->local.descriptor->hardware_id())) {
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
        libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
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

auto dispatch(CpuRuntime& runtime) noexcept -> bool {
    libk_assert(runtime.kernel && runtime.owner_registry && runtime.local.descriptor);
    auto& k = *runtime.kernel;
    auto& cpus = *runtime.owner_registry;
    const auto cpu = runtime.local.descriptor->logical_id();
    const auto budget = k.clock().duration_from_nanoseconds(1'000'000);
    const auto period = k.clock().duration_from_nanoseconds(10'000'000);
    libk_assert(budget && period);
    DispatchState state;
    std::array<DispatchArg, 3> args{};
    std::array<object::ref<Thread>, 3> threads{};
    std::array<object::ref<sched::Sc>, 3> scs{};
    constexpr std::array<u8, 3> priority{2, 5, 5};
    for (usize i = 0; i < threads.size(); ++i) {
        args[i] = {&state, i};
        auto stack = mm::Stack::create(k.kernel_vspace());
        libk_assert(stack);
        auto t = k.pool<Thread>().create(std::move(*stack), Env::kernel(k.kernel_vspace()),
                                       Thread::KernelStart{dispatch_entry, &args[i]});
        libk_assert(t);
        threads[i] = std::move(*t).publish();
        auto c = k.pool<sched::Sc>().create(sched::Sc::Config{
            .budget=*budget, .period=*period, .urgency=*sched::Urgency::make(priority[i])},
            k.clock().now());
        libk_assert(c);
        scs[i] = std::move(*c).publish();
        auto ref = threads[i].clone();
        libk_assert(ref && k.kernel_domain().admit(scs[i].get(), cpu)
                    && scs[i]->bind(std::move(*ref)));
    }
    // Publish all candidates before dispatch so urgency and equal-priority
    // FIFO are tested through real context switches, not a queue fixture.
    const bool enabled = arch::disable_interrupts();
    for (auto& sc : scs) libk_assert(sched::start(cpus, sc.get()));
    sched::yield();
    arch::restore_interrupts(enabled);
    const auto duration = k.clock().duration_from_nanoseconds(100'000'000);
    const auto until = duration ? k.clock().now().checked_add(*duration) : std::nullopt;
    libk_assert(until);
    for (auto& thread : threads) {
        while (!thread->stopped() && k.clock().now() < *until) sched::yield();
        libk_assert(thread->stopped());
    }
    const bool ordered = state.count == 3 && state.order == std::array<usize, 3>{1, 2, 0};
    for (usize i = 0; i < scs.size(); ++i) {
        libk_assert(!scs[i]->bound() && k.kernel_domain().unadmit(scs[i].get()));
        libk_assert(scs[i].retire() && threads[i].retire());
        scs[i].reset();
        threads[i].reset();
    }
    k.drain_reclaim();
    if (ordered) console::print<"[scenario] dispatch ok\n">();
    return ordered;
}

} // namespace test::scenario
