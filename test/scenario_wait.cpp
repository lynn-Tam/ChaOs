#include <test/scenario.hpp>

#include <core/kernel_state.hpp>
#include <cpu/cpu_runtime.hpp>
#include <cpu/cpu_registry.hpp>
#include <diag/console.hpp>
#include <mm/kernel_stack.hpp>
#include <sched/context.hpp>
#include <sched/dispatcher.hpp>
#include <sync/irq_lock_guard.hpp>
#include <thread/thread.hpp>

namespace kernel::test::scenario::detail {
namespace {
struct WaitState final { libk::Atomic<u32> ran{}; };

struct DeferredResult final {
    bool ready{}, cancelable{};
    usize releases{};
    operation::Completion completion{operation::Completion::bind<DeferredResult,
        &DeferredResult::complete, &DeferredResult::read, &DeferredResult::release,
        &DeferredResult::cancel>(*this)};

    auto complete() const noexcept -> bool { return ready; }
    auto read() noexcept -> operation::Result { KASSERT(ready); return {}; }
    void release() noexcept { ++releases; }
    auto cancel() noexcept -> bool { return cancelable && !ready; }
};

auto cancellation_publication(Thread& thread, CpuRegistry& cpus) noexcept -> bool {
    // This finite ordering check acts as producer and consumer on one stack.
    // A trap must not suspend it while its deliberately unpublished wait is
    // attached; ordinary asynchronous producers run independently of waiters.
    kernel::sync::IrqToken interrupts;
    {
        DeferredResult source;
        if (!thread.begin_wait(source.completion, cpus)) return false;
        source.ready = true;
        // A real producer may have stored its result and still owe signal().
        // Cancellation must retain its lifetime through that publication gap.
        if (thread.current_wait().cancel()) return false;
        if (!thread.current_wait().attached() || source.releases != 0) return false;
        source.completion.signal();
        thread.cancel_wait();
        if (thread.current_wait().attached() || source.releases != 1) return false;
    }
    {
        DeferredResult source;
        if (!thread.begin_wait(source.completion, cpus)) return false;
        source.ready = true;
        source.completion.signal();
        thread.cancel_wait();
        if (thread.current_wait().attached() || source.releases != 1) return false;
    }
    {
        DeferredResult source;
        source.cancelable = true;
        if (!thread.begin_wait(source.completion, cpus)) return false;
        thread.cancel_wait();
        if (thread.current_wait().attached() || source.releases != 1) return false;
    }
    return true;
}

void wait_entry(void* argument) noexcept {
    auto& state = *static_cast<WaitState*>(argument);
    auto& cpu = current_cpu();
    const bool passed = cancellation_publication(*cpu.current_thread(), *cpu.runtime().owner_registry);
    state.ran.store<libk::MemoryOrder::Release>(passed ? 1 : 2);
    sched::exit_current();
}
} // namespace

auto wait_publication(CpuRuntime& runtime) noexcept -> bool {
    if (runtime.local.descriptor == nullptr || runtime.kernel == nullptr
        || runtime.owner_registry == nullptr) {
        return false;
    }
    KernelState& kernel = *runtime.kernel;
    auto stack = KernelStack::create(kernel.kernel_vspace());
    if (!stack) {
        return false;
    }
    WaitState state{};
    auto pending_thread = kernel.objects().create_thread(
        libk::move(stack).value(),
        ExecutionBinding::kernel(kernel.kernel_vspace()),
        Thread::KernelStart{wait_entry, &state});
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

    sched::Binding* const binding = context->binding();
    const bool started = binding != nullptr
        && static_cast<bool>(sched::start(*runtime.owner_registry, *binding));
    if (started) {
        // Yield is a scheduling opportunity, not a join. Wait for the real
        // terminal state before unbinding and releasing the test's stack.
        while (thread->state() != Thread::State::Exited) sched::yield();
    }
    // Exited precedes the dispatcher's release of its binding. Keep both
    // objects alive until the asynchronous unbind has actually detached it.
    bool context_unbound = context->binding() == nullptr;
    while (!context_unbound) {
        context_unbound = static_cast<bool>(context->unbind());
        if (!context_unbound) sched::yield();
    }
    while (context->binding() != nullptr) sched::yield();
    const bool unadmitted = static_cast<bool>(
        kernel.kernel_domain().unadmit(context.get()));
    const bool context_retired = context.retire();
    const bool thread_retired = thread.retire();
    context.reset();
    thread.reset();
    kernel.objects().drain_reclaim();
    const bool result = started && state.ran.load<libk::MemoryOrder::Acquire>() == 1
        && context_unbound && unadmitted && context_retired && thread_retired;
    if (result) diag::console::print<"[scenario] wait publication ok\n">();
    return result;
}

} // namespace kernel::test::scenario::detail
