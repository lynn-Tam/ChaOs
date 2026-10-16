#include <expected>
#include <test/scenario.hpp>
#include <test/boot.hpp>

#include <cpu.hpp>
#include <boot/start.hpp>
#include <cpu/cpu.hpp>
#include <console.hpp>
#include <utility>
#include <mm/kspace.hpp>
#include <sched/sc.hpp>
#include <sched/dispatcher.hpp>
#include <task/thread.hpp>

namespace test::scenario {
namespace {

struct RemoteState final {
    std::atomic<u32> entered{};
    std::atomic<u32> release{};
    std::atomic<u32> returned{};
};

void remote_entry(void* argument) noexcept {
    auto& state = *static_cast<RemoteState*>(argument);
    state.entered.store(1, std::memory_order_release);
    while (state.release.load(std::memory_order_acquire) == 0) {
        std::atomic_signal_fence(std::memory_order_seq_cst);
    }
    // Thread::start() performs the scheduler Exit commit after this entry
    // returns.  This second marker therefore brackets the production task
    // entry rather than fabricating a local queue completion.
    state.returned.store(1, std::memory_order_release);
}

[[nodiscard]] auto remote_target(
    Cpu& runtime) noexcept -> Cpu* {
    if (runtime.cpus == nullptr) {
        return nullptr;
    }
    Cpus& cpus = *runtime.cpus;
    const CpuId boot = runtime.id;
    for (usize offset = 1; offset < cpus.count(); ++offset) {
        const CpuId id{(boot.raw + offset) % cpus.count()};
        Cpu* const candidate = cpus.get(id);
        if (candidate != nullptr && candidate->online()) {
            return candidate;
        }
    }
    return nullptr;
}

} // namespace

auto remote(Cpu& runtime) noexcept -> bool {
    Cpu* const target = remote_target(runtime);
    if (target == nullptr || test::boot == nullptr) {
        return false;
    }
    Boot& kernel = *test::boot;
    RemoteState state{};
    auto stack = mm::Stack::create(kernel.vm);
    if (!stack) {
        return false;
    }
    auto pending_thread = kernel.objects.get<Thread>().create(
        std::move(stack).value(),
        Env::kernel(kernel.vm),
        Thread::KernelStart{remote_entry, &state});
    if (!pending_thread) {
        return false;
    }
    auto thread = std::move(pending_thread).value().publish();
    const auto budget = kernel.clock.ticks<1, time::ms>();
    const auto period = kernel.clock.ticks<10, time::ms>();
    if (!budget || !period) {
        static_cast<void>(thread.destroy());
        thread.reset();

        return false;
    }
    auto pending_context = kernel.objects.get<sched::Sc>().create(
        sched::Sc::Config{.budget = *budget, .period = *period},
        kernel.clock.rd_ticks());
    if (!pending_context) {
        static_cast<void>(thread.destroy());
        thread.reset();

        return false;
    }
    auto context = std::move(pending_context).value().publish();
    auto admitted = kernel.domain.get().admit(
        context.get(), target->id);
    auto target_ref = thread;
    if (!admitted || !target_ref
        || !thread->claim_home(target->dispatcher())
        || !context->bind(context, thread.get())) {
        if (admitted) {
            static_cast<void>(kernel.domain.get().unadmit(context.get()));
        }
        static_cast<void>(context.destroy());
        context.reset();
        static_cast<void>(thread.destroy());
        thread.reset();

        return false;
    }

    sched::Sc* const binding = &context.get();
    if (binding == nullptr) {
        return false;
    }
    fail_ipis(2);
    const auto started = thread->execution(true);
    fail_ipis(0);
    const auto posted = started
        ? sched::wake(*runtime.cpus, thread.get())
        : sched::Dispatcher::WakeResult{std::unexpected(
              sched::Dispatcher::WakeError::Unavailable)};
    constexpr usize wait_spins = 1U << 22;
    bool entered = state.entered.load(std::memory_order_acquire) != 0;
    for (usize spin = 0; !entered && spin < wait_spins; ++spin) {
        entered = state.entered.load(std::memory_order_acquire) != 0;
        std::atomic_signal_fence(std::memory_order_seq_cst);
    }
    state.release.store(1, std::memory_order_release);
    // Exited and an empty Sc binding precede the final home-owner release.
    // Observe the real completion cut instead of racing retirement against it.
    bool stopped = thread->stopped();
    for (usize spin = 0; entered && !stopped && spin < wait_spins; ++spin) {
        stopped = thread->stopped();
        std::atomic_signal_fence(std::memory_order_seq_cst);
    }
    const bool returned = state.returned.load(std::memory_order_acquire) != 0;
    const bool released = stopped && !context->bound();

    const bool unadmitted = static_cast<bool>(
        kernel.domain.get().unadmit(context.get()));
    const bool context_retired = context.destroy();
    const bool thread_retired = thread.destroy();
    context.reset();
    thread.reset();


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
