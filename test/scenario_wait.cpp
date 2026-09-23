#include <test/scenario.hpp>

#include <core/kernel_state.hpp>
#include <cpu/cpu_runtime.hpp>
#include <cpu/cpu_registry.hpp>
#include <diag/console.hpp>
#include <mm/kernel_stack.hpp>
#include <mm/vspace.hpp>
#include <libk/scope_guard.hpp>
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

struct VmPeer final {
    mm::VSpace& space;
    KernelState& kernel;
    libk::Atomic<bool> entered{}, release{}, expired{};
};

void vm_peer_entry(void* argument) noexcept {
    auto& peer = *static_cast<VmPeer*>(argument);
    const auto duration = peer.kernel.clock().duration_from_nanoseconds(1'000'000'000);
    KASSERT(duration);
    const auto deadline = peer.kernel.clock().now().checked_add(*duration);
    KASSERT(deadline);
    // Install the real SATP root before withholding its shootdown IPI. Never
    // manufacture an active-cpu bit or acknowledge a ticket from the test.
    sync::IrqToken interrupts;
    peer.space.translation().activate(current_cpu());
    peer.entered.store<libk::MemoryOrder::Release>(true);
    while (!peer.release.load<libk::MemoryOrder::Acquire>()) {
        if (peer.kernel.clock().now() >= *deadline) {
            peer.expired.store<libk::MemoryOrder::Release>(true);
            break;
        }
    }
    peer.kernel.kernel_vspace().translation().activate(current_cpu());
}

auto pending_vm_wait(CpuRuntime& runtime) noexcept -> bool {
    auto& kernel = *runtime.kernel;
    auto& cpus = *runtime.owner_registry;
    const auto local = runtime.local.descriptor->logical_id();
    libk::optional<CpuId> remote;
    for (usize i = 0; i != cpus.count(); ++i) {
        const auto* descriptor = cpus.descriptor(CpuId{i});
        if (descriptor && descriptor->state() == CpuState::Online && CpuId{i} != local) {
            remote.emplace(CpuId{i});
            break;
        }
    }
    if (!remote) return true; // The single-hart publication test still runs.
    auto space_pending = kernel.objects().create_vspace(kernel.kernel_vspace());
    KASSERT(space_pending);
    auto space = libk::move(space_pending).value().publish();
    auto memory_pending = kernel.objects().create_anonymous(mm::page_size,
        mm::AnonymousConfig{.access = mm::AccessMask::of(mm::Access::Read), .eager = true});
    KASSERT(memory_pending);
    auto memory = libk::move(memory_pending).value().publish();
    auto cleanup = libk::on_scope_exit([&]() noexcept {
        KASSERT(space.retire()); space.reset();
        KASSERT(memory.retire()); memory.reset();
        kernel.objects().drain_reclaim();
    });
    const mm::VmContext context{&cpus, local};
    const mm::VirtRange range{mm::VirtAddr{0x70000000}, mm::page_size};
    const auto access = mm::AccessMask::of(mm::Access::Read);
    auto reference = memory.ref();
    KASSERT(reference);
    auto mapped = space->map_kernel(context, space->root_key(), {range, {0, 1}, access},
        libk::move(reference).value(), memory.get(),
        {.range = {0, 1}, .access = access, .types = mm::MemoryTypes::of(mm::MemoryType::Normal)});
    KASSERT(mapped && mapped.value().status == mm::VmStatus::Complete);
    VmPeer peer{space.get(), kernel};
    auto stack = KernelStack::create(kernel.kernel_vspace());
    KASSERT(stack);
    auto thread_pending = kernel.objects().create_thread(libk::move(stack).value(),
        ExecutionBinding::kernel(kernel.kernel_vspace()), Thread::KernelStart{vm_peer_entry, &peer});
    KASSERT(thread_pending);
    auto thread = libk::move(thread_pending).value().publish();
    auto budget = kernel.clock().duration_from_nanoseconds(1'000'000);
    auto period = kernel.clock().duration_from_nanoseconds(10'000'000);
    KASSERT(budget && period);
    auto sc_pending = kernel.objects().create_context(
        sched::SchedulingContext::Config{.budget = *budget, .period = *period}, kernel.clock().now());
    KASSERT(sc_pending);
    auto sc = libk::move(sc_pending).value().publish();
    auto clone = thread.clone();
    KASSERT(clone && kernel.kernel_domain().admit(sc.get(), *remote));
    KASSERT(sc->bind(libk::move(clone).value()));
    KASSERT(sched::start(cpus, *sc->binding()));
    while (!peer.entered.load<libk::MemoryOrder::Acquire>()) sched::yield();
    bool passed{};
    {
        // This kernel continuation must consume/cancel its operation before
        // enabling traps; otherwise trap return would treat it as a syscall.
        sync::IrqToken interrupts;
        const auto unmapped = space->unmap_kernel(context, space->root_key(), range);
        KASSERT(unmapped && unmapped.value() == mm::VmStatus::Pending);
        auto target = space.ref();
        KASSERT(target);
        auto& current = *current_cpu().current_thread();
        auto* wait = current.current_wait().prepare_vm(libk::move(target).value(), space.get());
        KASSERT(wait && current.begin_wait(wait->completion(), cpus));
        wait->start();
        KASSERT(space->pending() && !wait->completion().complete() && !current.current_wait().ready());
        peer.release.store<libk::MemoryOrder::Release>(true);
        const auto duration = kernel.clock().duration_from_nanoseconds(1'000'000'000);
        KASSERT(duration);
        const auto deadline = kernel.clock().now().checked_add(*duration);
        KASSERT(deadline);
        while (!current.current_wait().ready() && kernel.clock().now() < *deadline)
            KASSERT(space->service(context));
        passed = current.current_wait().ready() && wait->completion().complete() && !space->pending()
            && !peer.expired.load<libk::MemoryOrder::Acquire>();
        KASSERT(passed);
        current.cancel_wait();
        KASSERT(!current.current_wait().attached());
    }
    while (thread->state() != Thread::State::Exited) sched::yield();
    // Exited and an unbound SC precede the dispatcher's final home release.
    execution::Stop stopped;
    stopped.start(thread.get());
    while (!stopped.complete()) sched::yield();
    while (sc->binding() != nullptr) { (void)sc->unbind(); sched::yield(); }
    KASSERT(kernel.kernel_domain().unadmit(sc.get()));
    KASSERT(sc.retire()); sc.reset();
    KASSERT(thread.retire()); thread.reset();
    if (passed) diag::console::print<"[scenario] pending VM wait completed after remote shootdown\n">();
    return passed;
}

void wait_entry(void* argument) noexcept {
    auto& state = *static_cast<WaitState*>(argument);
    auto& cpu = current_cpu();
    const bool passed = cancellation_publication(*cpu.current_thread(), *cpu.runtime().owner_registry)
        && pending_vm_wait(cpu.runtime());
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
