#include <expected>
#include <test/scenario.hpp>
#include <test/boot.hpp>

#include <cpu.hpp>
#include <state.hpp>
#include <cpu/registry.hpp>
#include <cpu/runtime.hpp>
#include <console.hpp>
#include <utility>
#include <mm/kspace.hpp>
#include <sched/sc.hpp>
#include <sched/dispatcher.hpp>
#include <task/thread.hpp>

namespace test::scenario {
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
    auto stack = mm::Stack::create(kernel.kernel_vspace());
    if (!stack) {
        return false;
    }
    auto pending_thread = kernel.pool<Thread>().create(
        std::move(stack).value(),
        Env::kernel(kernel.kernel_vspace()),
        Thread::KernelStart{remote_entry, &state});
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
    auto pending_context = kernel.pool<sched::Sc>().create(
        sched::Sc::Config{.budget = *budget, .period = *period},
        kernel.clock().now());
    if (!pending_context) {
        static_cast<void>(thread.retire());
        thread.reset();
        kernel.drain_reclaim();
        return false;
    }
    auto context = std::move(pending_context).value().publish();
    auto admitted = kernel.kernel_domain().admit(
        context.get(), target->local.descriptor->logical_id());
    auto target_ref = thread.clone();
    if (!admitted || !target_ref
        || !context->bind(std::move(target_ref).value())) {
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
    if (binding == nullptr) {
        return false;
    }
    fail_ipis(2);
    const auto started = sched::start(*runtime.owner_registry, *binding);
    fail_ipis(0);
    const auto posted = started
        ? sched::wake(*runtime.owner_registry, *binding)
        : decltype(started){std::unexpected(
              sched::Dispatcher::WakeError::Unavailable)};
    constexpr usize wait_spins = 1U << 22;
    bool entered = state.entered.load<libk::MemoryOrder::Acquire>() != 0;
    for (usize spin = 0; !entered && spin < wait_spins; ++spin) {
        entered = state.entered.load<libk::MemoryOrder::Acquire>() != 0;
        libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
    }
    state.release.store<libk::MemoryOrder::Release>(1);
    // Exited and an empty Sc binding precede the final home-owner release.
    // Observe the real completion cut instead of racing retirement against it.
    bool stopped = thread->stopped();
    for (usize spin = 0; entered && !stopped && spin < wait_spins; ++spin) {
        stopped = thread->stopped();
        libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
    }
    const bool returned = state.returned.load<libk::MemoryOrder::Acquire>() != 0;
    const bool released = stopped && !context->bound();

    const bool unadmitted = static_cast<bool>(
        kernel.kernel_domain().unadmit(context.get()));
    const bool context_retired = context.retire();
    const bool thread_retired = thread.retire();
    context.reset();
    thread.reset();
    kernel.drain_reclaim();

    const bool result = started && posted && entered && returned && stopped
        && released && unadmitted
        && context_retired && thread_retired;
    if (result) {
        console::print<"[scenario] remote-delivery ok\n">();
    } else {
        console::print<"[scenario] remote failure: start={} wake={} enter={} return={} stopped={} released={} unadmit={} retire={}/{}\n">(
            bool(started), bool(posted), entered, returned, stopped,
            released, unadmitted, context_retired, thread_retired);
    }
    return result;
}

} // namespace test::scenario
