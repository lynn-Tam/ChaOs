#include <test/scenario.hpp>

#include <arch/boot_stack.hpp>
#include <arch/ipi.hpp>
#include <core/kernel_state.hpp>
#include <cpu/cpu_runtime.hpp>
#include <cpu/cpu_registry.hpp>
#include <diag/concurrency.hpp>
#include <diag/console.hpp>
#include <mm/kernel_stack.hpp>
#include <sched/context.hpp>
#include <sched/dispatcher.hpp>
#include <thread/thread.hpp>

namespace kernel::test::scenario::detail {

auto ordinary(const boot::BootInfo& boot) noexcept -> bool {
    const bool result = boot.cpu
        && boot.cpu.summary().count != 0
        && boot.timebase_frequency != 0
        && boot.fdt
        && arch_boot_stack_guard_intact();
    if (result) {
        diag::console::print<"[scenario] ordinary ok\n">();
    }
    return result;
}

auto initrd(const boot::BootInfo& boot) noexcept -> bool {
    const bool result = boot.module
        && boot.module->size != 0
        && boot.module->pages.valid();
    if (result) {
        diag::console::print<"[scenario] initrd ok\n">();
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
            && diag::concurrency::flight_head(*candidate) != 0) {
            target = candidate;
            break;
        }
    }
    if (target == nullptr || !arch::ipi_available()) {
        return false;
    }
    const u64 before = diag::concurrency::flight_head(*target);
    if (!arch::send_ipi(target->local.descriptor->hardware_id())) {
        return false;
    }

    bool entered{};
    bool exited{};
    for (u32 spin = 0; spin < (1U << 20); ++spin) {
        const usize count = diag::concurrency::flight_count(*target);
        diag::concurrency::FlightRecordValue value{};
        for (usize index = 0; index < count; ++index) {
            if (!diag::concurrency::flight_read(*target, index, value)
                || value.absolute_id < before) {
                continue;
            }
            entered = entered
                || value.event == diag::concurrency::FlightEvent::TrapEnter;
            exited = exited
                || value.event == diag::concurrency::FlightEvent::TrapExit;
        }
        if (entered && exited) {
            diag::console::print<"[scenario] trap ok\n">();
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
    auto stack = KernelStack::create(kernel.kernel_vspace());
    if (!stack) {
        return false;
    }
    DispatchState state{};
    auto pending_thread = kernel.objects().create_thread(
        libk::move(stack).value(),
        ExecutionBinding::kernel(kernel.kernel_vspace()),
        Thread::KernelStart{dispatch_entry, &state});
    if (!pending_thread) {
        return false;
    }
    auto thread = libk::move(pending_thread).value().publish();
    const auto budget = kernel.clock().duration_from_nanoseconds(1'000'000);
    const auto period = kernel.clock().duration_from_nanoseconds(10'000'000);
    if (!budget || !period) {
        static_cast<void>(thread.retire());
        thread.reset();
        kernel.objects().drain_reclaim();
        return false;
    }
    auto pending_context = kernel.objects().create_context(
        sched::SchedulingContext::Config{.budget = *budget, .period = *period},
        kernel.clock().now());
    if (!pending_context) {
        static_cast<void>(thread.retire());
        thread.reset();
        kernel.objects().drain_reclaim();
        return false;
    }
    auto context = libk::move(pending_context).value().publish();
    const CpuId cpu = runtime.local.descriptor->logical_id();
    auto admitted = kernel.kernel_domain().admit(context.get(), cpu);
    auto target = thread.clone();
    if (!admitted || !target
        || !context->bind(libk::move(target).value())) {
        if (context->admitted()) {
            static_cast<void>(kernel.kernel_domain().unadmit(context.get()));
        }
        static_cast<void>(context.retire());
        context.reset();
        static_cast<void>(thread.retire());
        thread.reset();
        kernel.objects().drain_reclaim();
        return false;
    }

    if (diag::concurrency::flight_head(runtime) == 0) {
        return false;
    }
    const u64 before = diag::concurrency::flight_head(runtime);
    sched::Binding* const binding = context->binding();
    const bool started = binding != nullptr
        && static_cast<bool>(sched::start(*runtime.owner_registry, *binding));
    if (started) {
        sched::yield();
    }
    bool saw_dispatch{};
    bool saw_exit{};
    if (diag::concurrency::flight_head(runtime) >= before) {
        const usize count = diag::concurrency::flight_count(runtime);
        diag::concurrency::FlightRecordValue value{};
        for (usize index = 0; index < count; ++index) {
            if (!diag::concurrency::flight_read(runtime, index, value)
                || value.absolute_id < before) {
                continue;
            }
            saw_dispatch = saw_dispatch
                || value.event == diag::concurrency::FlightEvent::Start
                || value.event == diag::concurrency::FlightEvent::Dispatch
                || value.event == diag::concurrency::FlightEvent::Yield;
            saw_exit = saw_exit
                || value.event == diag::concurrency::FlightEvent::Exit;
        }
    }
    const bool context_unbound = context->binding() == nullptr
        || static_cast<bool>(context->unbind());
    const bool unadmitted = static_cast<bool>(
        kernel.kernel_domain().unadmit(context.get()));
    const bool context_retired = context.retire();
    const bool thread_retired = thread.retire();
    context.reset();
    thread.reset();
    kernel.objects().drain_reclaim();
    // The handoff-in event may be overwritten by the bounded flight ring
    // while finish_exit publishes the returned idle target. The entry flag
    // proves the context switch reached the private task; retain the later
    // Exit commit as the durable scheduler witness.
    const bool handoff_commit = saw_dispatch || saw_exit;
    const bool result = started
        && state.ran.load<libk::MemoryOrder::Acquire>() != 0
        && handoff_commit && saw_exit && context_unbound && unadmitted
        && context_retired && thread_retired;
    if (result) {
        diag::console::print<"[scenario] dispatch-flight ok\n">();
    }
    return result;
}

} // namespace kernel::test::scenario::detail
