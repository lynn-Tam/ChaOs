#include <utility>
#include <test/scenario.hpp>

#include <arch/boot_stack.hpp>
#include <arch/ipi.hpp>
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
        && arch_boot_stack_guard_intact();
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

struct DispatchState final {
    libk::Atomic<u32> ran{};
};

void dispatch_entry(void* argument) noexcept {
    auto& state = *static_cast<DispatchState*>(argument);
    state.ran.store<libk::MemoryOrder::Release>(1);
    sched::exit_current();
}

} // namespace

auto dispatch(CpuRuntime& runtime) noexcept -> bool {
    if (runtime.local.descriptor == nullptr || runtime.kernel == nullptr
        || runtime.owner_registry == nullptr) {
        return false;
    }
    KernelState& kernel = *runtime.kernel;
    auto stack = mm::Stack::create(kernel.kernel_vspace());
    if (!stack) {
        return false;
    }
    DispatchState state{};
    auto pending_thread = kernel.tasks().threads.create(
        std::move(stack).value(),
        Env::kernel(kernel.kernel_vspace()),
        Thread::KernelStart{dispatch_entry, &state});
    if (!pending_thread) {
        return false;
    }
    auto thread = std::move(pending_thread).value().publish();
    const auto budget = kernel.clock().duration_from_nanoseconds(1'000'000);
    const auto period = kernel.clock().duration_from_nanoseconds(10'000'000);
    if (!budget || !period) {
        static_cast<void>(thread.retire());
        thread.reset();
        kernel.drain_reclaim();
        return false;
    }
    auto pending_context = kernel.sched().contexts.create(
        sched::Sc::Config{.budget = *budget, .period = *period},
        kernel.clock().now());
    if (!pending_context) {
        static_cast<void>(thread.retire());
        thread.reset();
        kernel.drain_reclaim();
        return false;
    }
    auto context = std::move(pending_context).value().publish();
    const CpuId cpu = runtime.local.descriptor->logical_id();
    auto admitted = kernel.kernel_domain().admit(context.get(), cpu);
    auto target = thread.clone();
    if (!admitted || !target
        || !context->bind(std::move(target).value())) {
        if (context->admitted()) {
            static_cast<void>(kernel.kernel_domain().unadmit(context.get()));
        }
        static_cast<void>(context.retire());
        context.reset();
        static_cast<void>(thread.retire());
        thread.reset();
        kernel.drain_reclaim();
        return false;
    }

    sched::Sc* const binding = &context.get();
    const bool started = binding != nullptr
        && static_cast<bool>(sched::start(*runtime.owner_registry, *binding));
    if (started) {
        sched::yield();
    }
    const bool context_unbound = !context->bound()
        || static_cast<bool>(context->unbind());
    const bool unadmitted = static_cast<bool>(
        kernel.kernel_domain().unadmit(context.get()));
    const bool context_retired = context.retire();
    const bool thread_retired = thread.retire();
    context.reset();
    thread.reset();
    kernel.drain_reclaim();
    const bool result = started
        && state.ran.load<libk::MemoryOrder::Acquire>() != 0
        && context_unbound && unadmitted
        && context_retired && thread_retired;
    if (result) {
        console::print<"[scenario] dispatch ok\n">();
    }
    return result;
}

} // namespace test::scenario
