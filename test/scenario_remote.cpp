#include <test/scenario.hpp>

#include <arch/interrupt.hpp>
#include <core/kernel_state.hpp>
#include <cpu/cpu_registry.hpp>
#include <cpu/cpu_runtime.hpp>
#include <diag/concurrency.hpp>
#include <diag/console.hpp>
#include <libk/utility.hpp>
#include <mm/kernel_stack.hpp>
#include <sched/context.hpp>
#include <sched/dispatcher.hpp>
#include <thread/thread.hpp>

namespace kernel::test::scenario::detail {
namespace {

struct RemoteState final {
    libk::Atomic<u32> entered{};
    libk::Atomic<u32> release{};
    libk::Atomic<u32> returned{};
};

void remote_entry(void* argument) noexcept {
    auto& state = *static_cast<RemoteState*>(argument);
    state.entered.store<libk::MemoryOrder::Release>(1);
    while (state.release.load<libk::MemoryOrder::Acquire>() == 0) {
        libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
    }
    // Thread::start() performs the scheduler Exit commit after this entry
    // returns.  This second marker therefore brackets the production task
    // entry rather than fabricating a local queue completion.
    state.returned.store<libk::MemoryOrder::Release>(1);
}

[[nodiscard]] auto remote_target(
    CpuRuntime& runtime) noexcept -> CpuRuntime* {
    if (runtime.owner_registry == nullptr || runtime.local.descriptor == nullptr) {
        return nullptr;
    }
    CpuRegistry& cpus = *runtime.owner_registry;
    const CpuId boot = runtime.local.descriptor->logical_id();
    for (usize offset = 1; offset < cpus.count(); ++offset) {
        const CpuId id{(boot.raw + offset) % cpus.count()};
        const CpuDescriptor* const descriptor = cpus.descriptor(id);
        CpuRuntime* const candidate = cpus.runtime(id);
        if (descriptor != nullptr && candidate != nullptr
            && descriptor->state() == CpuState::Online) {
            return candidate;
        }
    }
    return nullptr;
}

} // namespace

auto remote(CpuRuntime& runtime) noexcept -> bool {
    CpuRuntime* const target = remote_target(runtime);
    if (target == nullptr || runtime.kernel == nullptr) {
        return false;
    }
    KernelState& kernel = *runtime.kernel;
    RemoteState state{};
    auto stack = KernelStack::create(kernel.kernel_vspace());
    if (!stack) {
        return false;
    }
    auto pending_thread = kernel.objects().create_thread(
        libk::move(stack).value(),
        ExecutionBinding::kernel(kernel.kernel_vspace()),
        Thread::KernelStart{remote_entry, &state});
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
    auto admitted = kernel.kernel_domain().admit(
        context.get(), target->local.descriptor->logical_id());
    auto target_ref = thread.clone();
    if (!admitted || !target_ref
        || !context->bind(libk::move(target_ref).value())) {
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
    if (binding == nullptr) {
        return false;
    }
    // Reserve the target actor before posting. The producer only observes
    // this typed projection; the target dispatcher remains the sole writer
    // of its canonical wake credit and consumes the actual RemoteQueue/IPI.
    const auto actor = binding->actor_ref();
    const auto actor_key = diag::concurrency::ObservationKey{
        actor.identity};
    const auto started = sched::start(*runtime.owner_registry, *binding);
    const auto posted = started
        ? sched::wake(*runtime.owner_registry, *binding)
        : decltype(started){libk::unexpected(
              sched::CpuDispatcher::WakeError::Unavailable)};
    bool accepted = false;
    diag::concurrency::ObservationSnapshot snapshot{};
    if (posted && actor_key) {
        for (u32 spin = 0; spin < (1U << 20); ++spin) {
            if (diag::concurrency::observation_snapshot(
                    *target, actor_key, snapshot)
                && (snapshot.detail[0] & 4U) != 0) {
                accepted = true;
                break;
            }
            libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
        }
    }

    constexpr usize wait_spins = 1U << 22;
    bool entered = state.entered.load<libk::MemoryOrder::Acquire>() != 0;
    for (usize spin = 0; !entered && spin < wait_spins; ++spin) {
        entered = state.entered.load<libk::MemoryOrder::Acquire>() != 0;
        libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
    }
    state.release.store<libk::MemoryOrder::Release>(1);
    bool returned = state.returned.load<libk::MemoryOrder::Acquire>() != 0;
    for (usize spin = 0; entered && !returned && spin < wait_spins; ++spin) {
        returned = state.returned.load<libk::MemoryOrder::Acquire>() != 0;
        libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
    }
    bool exited = thread->state() == Thread::State::Exited;
    for (usize spin = 0; returned && !exited && spin < wait_spins; ++spin) {
        exited = thread->state() == Thread::State::Exited;
        libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
    }
    // Thread::start() performs the scheduler Exit commit after the entry
    // returns. Once that production-owned state is visible, explicitly tear
    // down the context binding and wait for the target to release it before
    // unadmit/retire can touch either object.
    bool unbound{};
    for (usize spin = 0; exited && !unbound && spin < wait_spins; ++spin) {
        unbound = context->binding() == nullptr
            || static_cast<bool>(context->unbind());
        if (!unbound) {
            libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
        }
    }
    bool released = context->binding() == nullptr;
    for (usize spin = 0; unbound && !released && spin < wait_spins; ++spin) {
        released = context->binding() == nullptr;
        libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
    }

    const bool unadmitted = static_cast<bool>(
        kernel.kernel_domain().unadmit(context.get()));
    const bool context_retired = context.retire();
    const bool thread_retired = thread.retire();
    context.reset();
    thread.reset();
    kernel.objects().drain_reclaim();

    const bool result = started && posted && accepted && entered && returned && exited
        && released && unbound && unadmitted
        && context_retired && thread_retired;
    if (result) {
        diag::console::print<"[scenario] remote-delivery ok\n">();
    }
    return result;
}

} // namespace kernel::test::scenario::detail
