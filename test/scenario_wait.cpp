#include <libk/mem.h>
#include <optional>
#include <utility>
#include <test/scenario.hpp>

#include <state.hpp>
#include <cpu/runtime.hpp>
#include <cpu/registry.hpp>
#include <console.hpp>
#include <mm/kspace.hpp>
#include <mm/vspace.hpp>
#include <libk/scope_guard.hpp>
#include <sched/sc.hpp>
#include <sched/dispatcher.hpp>
#include <sync.hpp>
#include <task/thread.hpp>
#include <mm/mem.hpp>

namespace test::scenario {
namespace {
struct WaitState final {
    libk::Atomic<u32> ran{};
};

struct DeferredResult final {
    bool ready{}, cancelable{};
    usize releases{};
    Completion completion{
        Completion::bind<DeferredResult, &DeferredResult::release, &DeferredResult::cancel>(*this)};

    void release() noexcept { ++releases; }
    auto cancel() noexcept -> bool { return cancelable && !ready; }
};

auto cancellation_publication(Thread& thread, CpuRegistry& cpus) noexcept -> bool {
    // This finite ordering check acts as producer and consumer on one stack.
    // A trap must not suspend it while its deliberately unpublished wait is
    // attached; ordinary asynchronous producers run independently of waiters.
    sync::Irq interrupts;
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
    libk_assert(duration);
    const auto deadline = peer.kernel.clock().now().checked_add(*duration);
    libk_assert(deadline);
    // Install the real SATP root before withholding its shootdown IPI. Never
    // manufacture an active-cpu bit or acknowledge a ticket from the test.
    sync::Irq interrupts;
    peer.space.root().activate(current_cpu());
    peer.entered.store<libk::MemoryOrder::Release>(true);
    while (!peer.release.load<libk::MemoryOrder::Acquire>()) {
        if (peer.kernel.clock().now() >= *deadline) {
            peer.expired.store<libk::MemoryOrder::Release>(true);
            break;
        }
    }
    peer.kernel.kernel_vspace().root().activate(current_cpu());
}

auto pending_vm_wait(CpuRuntime& runtime) noexcept -> bool {
    auto& kernel = *runtime.kernel;
    auto& cpus = *runtime.owner_registry;
    const auto local = runtime.local.descriptor->logical_id();
    std::optional<CpuId> remote;
    for (usize i = 0; i != cpus.count(); ++i) {
        const auto* descriptor = cpus.descriptor(CpuId{i});
        if (descriptor && descriptor->state() == CpuState::Online && CpuId{i} != local) {
            remote.emplace(CpuId{i});
            break;
        }
    }
    if (!remote) return true; // The single-hart publication test still runs.
    auto space_pending = kernel.pool<mm::VSpace>().make(
        {}, [&](auto& m) { return m.initialize(); }, kernel.pmm(), kernel.kernel_vspace(),
        kernel.space_work());
    libk_assert(space_pending);
    auto space = std::move(space_pending).value().publish();
    auto memory_pending = kernel.pool<mm::Mem>().make(
        {},
        [&](auto& m) {
            return m.init_anon(
                mm::AnonCfg{.access = mm::Perms::of(mm::Perm::Read), .eager = true});
        },
        kernel.pmm(), mm::page_size);
    libk_assert(memory_pending);
    auto memory = std::move(memory_pending).value().publish();
    auto cleanup = libk::on_scope_exit([&]() noexcept {
        libk_assert(space.retire());
        space.reset();
        libk_assert(memory.retire());
        memory.reset();
        kernel.drain_reclaim();
    });
    const mm::VmCtx context{&cpus, local};
    const mm::VRange range{mm::Virt{0x70000000}, mm::page_size};
    const auto access = mm::Perms::of(mm::Perm::Read);
    auto reference = memory.erase();
    libk_assert(reference);
    auto mapped =
        space->map(context, {range, {0, 1}, access}, std::move(reference).value(), memory.get(),
                   {.range = {0, 1}, .access = access, .types = mm::MemoryTypes::of(mm::MemoryType::Normal)});
    libk_assert(mapped && mapped.value().status == mm::VmStatus::Complete);
    VmPeer peer{space.get(), kernel};
    auto stack = mm::Stack::create(kernel.kernel_vspace());
    libk_assert(stack);
    auto thread_pending =
        kernel.tasks().threads.create(std::move(stack).value(), Env::kernel(kernel.kernel_vspace()),
                                      Thread::KernelStart{vm_peer_entry, &peer});
    libk_assert(thread_pending);
    auto thread = std::move(thread_pending).value().publish();
    auto budget = kernel.clock().duration_from_nanoseconds(1'000'000);
    auto period = kernel.clock().duration_from_nanoseconds(10'000'000);
    libk_assert(budget && period);
    auto sc_pending = kernel.sched().contexts.create(sched::Sc::Config{.budget = *budget, .period = *period},
                                                     kernel.clock().now());
    libk_assert(sc_pending);
    auto sc = std::move(sc_pending).value().publish();
    auto clone = thread.clone();
    libk_assert(clone && kernel.kernel_domain().admit(sc.get(), *remote));
    libk_assert(sc->bind(std::move(clone).value()));
    libk_assert(sched::start(cpus, sc.get()));
    while (!peer.entered.load<libk::MemoryOrder::Acquire>()) sched::yield();
    bool passed{};
    {
        // This kernel continuation must consume/cancel its operation before
        // enabling traps while this finite observer owns an attached wait.
        sync::Irq interrupts;
        const auto unmapped = space->unmap(context, range);
        libk_assert(unmapped && unmapped.value() == mm::VmStatus::Pending);
        auto target = space.erase();
        libk_assert(target);
        auto& current = *current_cpu().current_thread();
        mm::Fence wait{std::move(target).value(), space.get()};
        libk_assert(current.begin_wait(wait.completion(), cpus));
        wait.start();
        libk_assert(space->pending() && !current.current_wait().ready());
        peer.release.store<libk::MemoryOrder::Release>(true);
        const auto duration = kernel.clock().duration_from_nanoseconds(1'000'000'000);
        libk_assert(duration);
        const auto deadline = kernel.clock().now().checked_add(*duration);
        libk_assert(deadline);
        while (!current.current_wait().ready() && kernel.clock().now() < *deadline)
            libk_assert(space->service(context));
        passed = current.current_wait().ready() && !space->pending() &&
                 !peer.expired.load<libk::MemoryOrder::Acquire>();
        libk_assert(passed);
        current.cancel_wait();
        libk_assert(!current.current_wait().attached());
    }
    while (thread->state() != Thread::State::Exited) sched::yield();
    // Exited and an unbound SC precede the dispatcher's final home release.
    Stop stopped;
    stopped.start(thread.get());
    while (!stopped.complete()) sched::yield();
    while (sc->bound()) {
        (void)sc->unbind();
        sched::yield();
    }
    libk_assert(kernel.kernel_domain().unadmit(sc.get()));
    libk_assert(sc.retire());
    sc.reset();
    libk_assert(thread.retire());
    thread.reset();
    if (passed) console::print<"[scenario] pending VM wait completed after remote shootdown\n">();
    return passed;
}

auto live_trim(CpuRuntime& rt) noexcept -> bool {
    // Native continuations enter the same IRQ-off blocking context as a syscall.
    sync::Irq interrupts;
    using namespace mm;
    auto& k = *rt.kernel;
    auto& cpus = *rt.owner_registry;
    auto pg = k.pool<Pager>().create();
    if (!pg) return false;
    auto pager = std::move(*pg).publish();
    auto pg_ref = pager.erase();
    if (!pg_ref) return false;
    auto pending = k.pool<mm::Mem>().make(
        {},
        [&](auto& m) {
            return m.init_paged(std::move(*pg_ref), Perms::of(Perm::Read, Perm::Write), false);
        },
        k.pmm(), page_size);
    if (!pending) return false;
    auto mem = std::move(*pending).publish();
    auto a = k.pool<mm::VSpace>().make(
             {}, [&](auto& m) { return m.initialize(); }, k.pmm(), k.kernel_vspace(), k.space_work()),
         b = k.pool<mm::VSpace>().make(
             {}, [&](auto& m) { return m.initialize(); }, k.pmm(), k.kernel_vspace(), k.space_work());
    if (!a || !b) return false;
    auto first = std::move(*a).publish(), second = std::move(*b).publish();
    auto cleanup = libk::on_scope_exit([&]() noexcept {
        (void)first.retire();
        (void)second.retire();
        first.reset();
        second.reset();
        (void)mem.retire();
        mem.reset();
        (void)pager.retire();
        pager.reset();
        k.drain_reclaim();
    });
    auto supply = [&]() noexcept {
        auto request = pager->claim();
        auto page = k.pmm().allocate_page();
        if (!request || !page || request->kind != Pager::Kind::PageIn) return false;
        memset(page->bytes(), 0x5a, page_size);
        return bool(mem->supply(pager.get(), request->id, std::move(*page)));
    };
    if (mem->materialize(0).error() != MemErr::Pending || !supply()) return false;
    const VmCtx ctx{&cpus, rt.local.descriptor->logical_id()};
    const VRange range{Virt{0x72000000}, page_size};
    const auto perms = Perms::of(Perm::Read, Perm::Write);
    MapId ids[2];
    usize i{};
    for (auto* space : {&first.get(), &second.get()}) {
        auto ref = mem.erase();
        if (!ref) return false;
        auto map =
            space->map(ctx, {range, {0, 1}, perms}, std::move(*ref), mem.get(),
                       {.range = {0, 1}, .access = perms, .types = MemoryTypes::of(MemoryType::Normal)});
        if (!map) return false;
        auto fault = space->fault(ctx, range.base(), Perm::Read);
        if (!fault || fault->kind != FaultKind::Materialized) return false;
        ids[i++] = map->mapping;
    }
    auto& thread = *current_cpu().current_thread();
    if (!mem->trim(thread, cpus, {0, 1}) || mem->state() != MemState::Live ||
        mem->query(0).value() != ContentState::Zero || !first->inspect(ids[0]) || !second->inspect(ids[1]))
        return false;
    // Both old logical Maps survive; a refault is a new real Pager request.
    auto fault = first->fault(ctx, range.base(), Perm::Read);
    if (!fault || fault->kind != FaultKind::Pending || !supply()) return false;
    fault = first->fault(ctx, range.base(), Perm::Read);
    if (!fault || fault->kind != FaultKind::Materialized) return false;
    auto held = mem->materialize(0);
    if (!held) return false;
    k.pmm().bytes(held->page().page)[0] = byte{0x6b};
    if (!mem->observe_usage(0, false, true)) return false;
    held->reset();
    auto dirty = mem->trim(thread, cpus, {0, 1});
    if (dirty || dirty.error() != MemErr::Dirty || mem->query(0).value() != ContentState::Resident ||
        !mem->writeback(0))
        return false;
    auto write = pager->claim();
    byte byte_read{};
    if (!write || write->kind != Pager::Kind::Writeback || !mem->read(0, {&byte_read, 1}) ||
        byte_read != byte{0x6b} || !mem->pager_finish(pager.get(), write->id, false))
        return false;
    if (!mem->trim(thread, cpus, {0, 1}) || mem->query(0).value() != ContentState::Zero) return false;
    console::print<"[scenario] live trim ok: two spaces, refault, dirty writeback\n">();
    return true;
}

void wait_entry(void* argument) noexcept {
    auto& state = *static_cast<WaitState*>(argument);
    auto& cpu = current_cpu();
    const bool passed = cancellation_publication(*cpu.current_thread(), *cpu.runtime().owner_registry) &&
                        pending_vm_wait(cpu.runtime()) && live_trim(cpu.runtime());
    state.ran.store<libk::MemoryOrder::Release>(passed ? 1 : 2);
    sched::exit_current();
}
} // namespace

auto wait_publication(CpuRuntime& runtime) noexcept -> bool {
    if (runtime.local.descriptor == nullptr || runtime.kernel == nullptr ||
        runtime.owner_registry == nullptr) {
        return false;
    }
    KernelState& kernel = *runtime.kernel;
    auto stack = mm::Stack::create(kernel.kernel_vspace());
    if (!stack) {
        return false;
    }
    WaitState state{};
    auto pending_thread =
        kernel.tasks().threads.create(std::move(stack).value(), Env::kernel(kernel.kernel_vspace()),
                                      Thread::KernelStart{wait_entry, &state});
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
        sched::Sc::Config{.budget = *budget, .period = *period}, kernel.clock().now());
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
    if (!admitted || !target || !context->bind(std::move(target).value())) {
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
    const bool started =
        binding != nullptr && static_cast<bool>(sched::start(*runtime.owner_registry, *binding));
    if (started) {
        // Yield is a scheduling opportunity, not a join. Wait for the real
        // terminal state before unbinding and releasing the test's stack.
        while (thread->state() != Thread::State::Exited) sched::yield();
    }
    // Exited precedes the dispatcher's release of its binding. Keep both
    // objects alive until the asynchronous unbind has actually detached it.
    bool context_unbound = !context->bound();
    while (!context_unbound) {
        context_unbound = static_cast<bool>(context->unbind());
        if (!context_unbound) sched::yield();
    }
    while (context->bound()) sched::yield();
    const bool unadmitted = static_cast<bool>(kernel.kernel_domain().unadmit(context.get()));
    const bool context_retired = context.retire();
    const bool thread_retired = thread.retire();
    context.reset();
    thread.reset();
    kernel.drain_reclaim();
    const bool result = started && state.ran.load<libk::MemoryOrder::Acquire>() == 1 && context_unbound &&
                        unadmitted && context_retired && thread_retired;
    if (result) console::print<"[scenario] wait publication ok\n">();
    return result;
}

} // namespace test::scenario
