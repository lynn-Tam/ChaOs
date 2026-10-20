#include <libk/mem.h>
#include <optional>
#include <utility>
#include <test/scenario.hpp>

#include <boot/start.hpp>
#include <test/boot.hpp>
#include <cpu/cpu.hpp>
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
    std::atomic<u32> ran{};
};

auto cancellation_publication(Thread& thread, Cpus& cpus) noexcept -> bool {
    sync::Irq interrupts;
    const Source::Context context{thread, cpus, current_cpu().dispatcher()};
    Source source;
    usize releases{}, drains{};
    Source::Action ready;
    auto release = [&]() noexcept { ++releases; };
    auto drain = [&]() noexcept { ++drains; };
    auto admission = [&](Source::Commit commit, Source::Action, Source::Action done) noexcept {
        ready = done; commit(Source::Action::bind(drain)); return STATUS_OK;
    };
    if (source.await(context, {}, {}, Source::Admission::bind(admission), Source::Action::bind(release)).status != STATUS_PENDING)
        return false;
    // Source has claimed a result, but real authority teardown still owes its
    // ready callback. Stop cannot release or reopen this fixed registration.
    if (!source.post(1) || drains != 1 || thread.wait().cancel() || releases || thread.wait().ready()) return false;
    ready(); ready = {};
    thread.cancel_wait();
    if (thread.waiting() || releases != 1 || thread.wait().result().value != 1) return false;
    (void)source.take();
    if (source.await(context, {}, {}, {}, Source::Action::bind(release)).status != STATUS_PENDING) return false;
    if (!source.post(1)) return false;
    thread.cancel_wait();
    if (thread.waiting() || releases != 2 || thread.wait().result().value != 1) return false;
    (void)source.take();
    if (source.await(context, {}, {}, {}, Source::Action::bind(release)).status != STATUS_PENDING) return false;
    thread.cancel_wait();
    return !thread.waiting() && releases == 3 && thread.wait().result().status == STATUS_CANCELED;
}

auto source_wait(Thread& thread, Cpus& cpus) noexcept -> bool {
    sync::Irq irq;
    Source source;
    auto& dispatcher = current_cpu().dispatcher();
    const auto seen = source.snapshot().sequence;
    if (source.await({thread, cpus, dispatcher}, {.mask = 0, .seen = seen, .changed = true}).status != STATUS_PENDING)
        return false;
    if (!source.update(0)) return false;
    thread.block();
    if (thread.wait().result().sequence == seen) return false;
    if (!source.post(3) || source.take(1).value != 1 || source.take().value != 2) return false;
    const auto deadline = dispatcher.clock().rd_ticks() + *dispatcher.clock().ticks<1, time::ms>();
    if (source.await({thread, cpus, dispatcher}, {}, deadline).status != STATUS_PENDING) return false;
    thread.block();
    if (thread.wait().result().status != STATUS_TIMED_OUT) return false;
    if (source.await({thread, cpus, dispatcher}, {}).status != STATUS_PENDING) return false;
    source.close();
    thread.block();
    return source.drained() && thread.wait().result().status == STATUS_CLOSED;
}

// Crossing the inline publication batch must preserve the same READY history.
auto source_history(Cpu& runtime) noexcept -> bool {
    auto& k = *test::boot;
    Src<SOURCE_READY> source;
    source.bind_work(k.work, {});
    std::array<object::ref<Thread>, 9> threads;
    for (usize i = 0; i < threads.size(); ++i) {
        auto stack = mm::Stack::create(k.vm); libk_assert(stack);
        auto made = k.objects.get<Thread>().create(std::move(*stack), Env::kernel(k.vm),
            Thread::KernelStart{[](void*) noexcept {}, nullptr});
        libk_assert(made); threads[i] = made->publish();
        auto& home = runtime.cpus->get(CpuId{i % runtime.cpus->count()})->dispatcher();
        libk_assert(threads[i]->claim_home(home));
        libk_assert(source.await({threads[i].get(), *runtime.cpus, home}, {.mask=SOURCE_READY}).status == STATUS_PENDING);
    }
    auto ready = source.prepare(SOURCE_READY);
    libk_assert(source.take().value == SOURCE_READY);
    source.close(); ready.publish();
    for (auto& thread : threads) {
        libk_assert(thread->wait().ready());
        const auto result = thread->wait().result();
        libk_assert(result.status == STATUS_OK && result.value == SOURCE_READY);
        thread->cancel_wait();
    }
    while (!source.drained()) sched::yield();
    libk_assert(source.await({*runtime.current, *runtime.cpus, runtime.dispatcher()}, {}).status == STATUS_CLOSED);
    for (auto& thread : threads) {
        libk_assert(thread.destroy());
        while (thread->state() != Thread::State::Exited) sched::yield();
        thread.reset();
    }
    return true;
}

struct VmPeer final {
    mm::VSpace& space;
    Boot& kernel;
    std::atomic<bool> entered{}, release{}, expired{};
};

void vm_peer_entry(void* argument) noexcept {
    auto& peer = *static_cast<VmPeer*>(argument);
    const auto duration = peer.kernel.clock.ticks<1, time::s>();
    libk_assert(duration);
    const auto deadline = libk::checked_add(peer.kernel.clock.rd_ticks(), *duration);
    libk_assert(deadline);
    // Install the real SATP root before withholding its shootdown IPI. Never
    // manufacture an active-cpu bit or acknowledge a ticket from the test.
    sync::Irq interrupts;
    peer.space.root().activate(current_cpu());
    peer.entered.store(true, std::memory_order_release);
    while (!peer.release.load(std::memory_order_acquire)) {
        if (peer.kernel.clock.rd_ticks() >= *deadline) {
            peer.expired.store(true, std::memory_order_release);
            break;
        }
    }
    peer.kernel.vm.root().activate(current_cpu());
}

auto pending_vm_wait(Cpu& runtime) noexcept -> bool {
    auto& kernel = *test::boot;
    auto& cpus = *runtime.cpus;
    const auto local = runtime.id;
    std::optional<CpuId> remote;
    for (usize i = 0; i != cpus.count(); ++i) {
        const auto* cpu = cpus.get(CpuId{i});
        if (cpu && cpu->online() && CpuId{i} != local) {
            remote.emplace(CpuId{i});
            break;
        }
    }
    if (!remote) return true; // The single-hart publication test still runs.
    auto space_pending = kernel.objects.get<mm::VSpace>().create(
        resource::Reservation{}, kernel.pmm, kernel.vm,
        kernel.work);
    libk_assert(space_pending);
    auto space = std::move(space_pending).value().publish();
    auto memory_pending = kernel.objects.get<mm::Mem>().create({}, kernel.pmm, mm::page_size, mm::AnonCfg{.perms = mm::Perms::of(mm::Perm::Read), .eager = true});
    libk_assert(memory_pending);
    auto memory = std::move(memory_pending).value().publish();
    auto cleanup = libk::on_scope_exit([&]() noexcept {
        libk_assert(space.destroy());
        space.reset();
        libk_assert(memory.destroy());
        memory.reset();

    });
    const mm::VmCtx context{&cpus, local};
    const mm::VRange range{mm::va_t{0x70000000}, mm::page_size};
    const auto access = mm::Perms::of(mm::Perm::Read);
    auto reference = object::ref<>{memory};
    libk_assert(reference);
    auto mapped =
        space->map(context, {range, {0, 1}, access}, std::move(reference), memory.get(),
                   {.range = {0, 1}, .perms = access});
    libk_assert(mapped && mapped.value().status == mm::VmStatus::Complete);
    const auto materialized = space->fault(context, range.base(), mm::Perm::Read);
    libk_assert(materialized && materialized->kind == mm::FaultKind::Materialized);
    VmPeer peer{space.get(), kernel};
    auto stack = mm::Stack::create(kernel.vm);
    libk_assert(stack);
    auto thread_pending =
        kernel.objects.get<Thread>().create(std::move(stack).value(), Env::kernel(kernel.vm),
                                      Thread::KernelStart{vm_peer_entry, &peer});
    libk_assert(thread_pending);
    auto thread = std::move(thread_pending).value().publish();
    auto budget = kernel.clock.ticks<1, time::ms>();
    auto period = kernel.clock.ticks<10, time::ms>();
    libk_assert(budget && period);
    auto sc_pending = kernel.objects.get<sched::Sc>().create(sched::Sc::Config{.budget = *budget, .period = *period},
                                                     kernel.clock.rd_ticks());
    libk_assert(sc_pending);
    auto sc = std::move(sc_pending).value().publish();
    auto clone = thread;
    libk_assert(clone && kernel.domain.get().admit(sc.get(), *remote));
    libk_assert(thread->claim_home(cpus.get(*remote)->dispatcher()));
    libk_assert(sc->bind(sc, thread.get()));
    libk_assert(thread->execution(true));
    while (!peer.entered.load(std::memory_order_acquire)) sched::yield();
    bool passed{};
    {
        sync::Irq interrupts;
        const auto unmapped = space->unmap(context, range);
        libk_assert(unmapped && unmapped.value() == mm::VmStatus::Pending);
        auto target = object::ref<>{space};
        libk_assert(target);
        auto& current = *current_cpu().current;
        libk_assert(space->wait_pending(current, cpus, current_cpu().dispatcher()).status == STATUS_PENDING);
        libk_assert(space->pending() && !current.wait().ready());
        peer.release.store(true, std::memory_order_release);
        const auto duration = kernel.clock.ticks<1, time::s>();
        libk_assert(duration);
        const auto deadline = libk::checked_add(kernel.clock.rd_ticks(), *duration);
        libk_assert(deadline);
        interrupts.restore();
        while (!current.wait().ready() && kernel.clock.rd_ticks() < *deadline)
            sched::yield();
        passed = current.wait().ready() && !space->pending() &&
                 !peer.expired.load(std::memory_order_acquire);
        libk_assert(passed);
        current.cancel_wait();
        libk_assert(!current.wait().attached());
    }
    while (thread->state() != Thread::State::Exited) sched::yield();
    // Exited and an unbound SC precede the dispatcher's final home release.
    thread->stop();
    while (!thread->stopped()) sched::yield();
    while (sc->bound()) {
        sched::yield();
    }
    libk_assert(kernel.domain.get().unadmit(sc.get()));
    libk_assert(sc.destroy());
    sc.reset();
    libk_assert(thread.destroy());
    thread.reset();
    if (passed) console::print<"[scenario] pending VM wait completed after remote shootdown\n">();
    return passed;
}

auto live_trim(Cpu& rt) noexcept -> bool {
    // Native continuations enter the same IRQ-off blocking context as a syscall.
    sync::Irq interrupts;
    using namespace mm;
    auto& k = *test::boot;
    auto& cpus = *rt.cpus;
    auto pg = k.objects.get<Pager>().create();
    if (!pg) return false;
    auto pager = std::move(*pg).publish();
    auto pg_ref = object::ref<>{pager};
    if (!pg_ref) return false;
    auto pending = k.objects.get<mm::Mem>().create({}, k.pmm, 2 * page_size, mm::PagedCfg{std::move(pg_ref), Perms::of(Perm::Read, Perm::Write), false});
    if (!pending) return false;
    auto mem = std::move(*pending).publish();
    auto a = k.objects.get<mm::VSpace>().create(
             resource::Reservation{}, k.pmm, k.vm, k.work),
         b = k.objects.get<mm::VSpace>().create(
             resource::Reservation{}, k.pmm, k.vm, k.work);
    if (!a || !b) return false;
    auto first = std::move(*a).publish(), second = std::move(*b).publish();
    auto cleanup = libk::on_scope_exit([&]() noexcept {
        (void)first.destroy();
        (void)second.destroy();
        first.reset();
        second.reset();
        (void)mem.destroy();
        mem.reset();
        (void)pager.destroy();
        pager.reset();

    });
    auto supply = [&]() noexcept {
        auto request = pager->claim();
        auto page = k.pmm.allocate_page();
        if (!request || !page || request->kind != Pager::Kind::PageIn) return false;
        memset(page->bytes(), 0x5a, page_size);
        return bool(mem->supply(pager.get(), request->id, std::move(*page)));
    };
    if (mem->materialize(0).error() != MemErr::Pending || !supply()
        || mem->materialize(1).error() != MemErr::Pending || !supply()) return false;
    const VmCtx ctx{&cpus, rt.id};
    const VRange range{va_t{0x72000000}, 2 * page_size};
    const auto perms = Perms::of(Perm::Read, Perm::Write);
    MapId ids[2];
    usize i{};
    for (auto* space : {&first.get(), &second.get()}) {
        auto ref = object::ref<>{mem};
        if (!ref) return false;
        auto map =
            space->map(ctx, {range, {0, 2}, perms}, std::move(ref), mem.get(),
                       {.range = {0, 2}, .perms = perms});
        if (!map) return false;
        auto fault = space->fault(ctx, range.base(), Perm::Read);
        if (!fault || fault->kind != FaultKind::Materialized) return false;
        fault = space->fault(ctx, va_t{range.base().raw() + page_size}, Perm::Read);
        if (!fault || fault->kind != FaultKind::Materialized) return false;
        ids[i++] = map->mapping;
    }
    auto borrowed = first->bind_view({.memory = mem, .object = {1, 1},
        .virtual_range = VRange{va_t{range.base().raw() + page_size}, page_size}});
    if (!borrowed) return false;
    auto& thread = *current_cpu().current;
    auto trim = [&](ObjectRange selected = {0, 1}) noexcept -> std::expected<void, MemErr> {
        Source::Context context{thread, cpus, current_cpu().dispatcher()};
        auto started = mem->trim(selected, context);
        if (started || started.error() != MemErr::Pending) return started;
        thread.block(); // This scenario driver is a kernel thread, not a provider.
        const auto result = thread.wait().result();
        if (result.status != STATUS_OK) return std::unexpected(MemErr::BackingFailed);
        if (result.value == MEM_TRIM_BUSY) return std::unexpected(MemErr::Busy);
        return result.value == MEM_TRIM_DIRTY ? std::expected<void, MemErr>{std::unexpected(MemErr::Dirty)}
            : std::expected<void, MemErr>{};
    };
    if (!trim() || mem->state() != MemState::Live ||
        mem->query(0).value() != ContentState::Zero || !first->inspect(ids[0]) || !second->inspect(ids[1]))
        return false;
    if (mem->query(1).value() != ContentState::Resident || !borrowed->valid()) return false;
    auto pinned = trim({1, 1});
    if (pinned || pinned.error() != MemErr::Busy || !borrowed->valid()) return false;
    // Both old logical Maps survive; a refault is a new real Pager request.
    auto fault = first->fault(ctx, range.base(), Perm::Read);
    if (!fault || fault->kind != FaultKind::Pending || !supply()) return false;
    fault = first->fault(ctx, range.base(), Perm::Read);
    if (!fault || fault->kind != FaultKind::Materialized) return false;
    auto held = mem->materialize(0);
    if (!held) return false;
    k.pmm.bytes(held->page())[0] = byte{0x6b};
    if (!mem->observe_usage(0, false, true)) return false;
    held->reset();
    auto dirty = trim();
    if (dirty || dirty.error() != MemErr::Dirty || mem->query(0).value() != ContentState::Resident ||
        !mem->writeback(0))
        return false;
    auto write = pager->claim();
    byte byte_read{};
    if (!write || write->kind != Pager::Kind::Writeback || !mem->read(0, {&byte_read, 1}) ||
        byte_read != byte{0x6b} || !mem->pager_finish(pager.get(), write->id, false))
        return false;
    if (!trim() || mem->query(0).value() != ContentState::Zero) return false;
    console::print<"[scenario] live trim ok: two spaces, partial range, wired Busy, refault, dirty writeback\n">();
    return true;
}

void wait_entry(void* argument) noexcept {
    auto& state = *static_cast<WaitState*>(argument);
    auto& cpu = current_cpu();
    // The worker's arm/post/block handshake has no user Wait to retain its
    // result. An intervening dispatch must preserve its single wake credit.
    libk_assert(sched::wake(*cpu.cpus, *cpu.current));
    sched::yield();
    sched::block();
    console::print<"[scenario] wake credit survives dispatch before block\n">();
    const bool passed = cancellation_publication(*cpu.current, *cpu.cpus) && source_wait(*cpu.current, *cpu.cpus) &&
                        pending_vm_wait(cpu) && live_trim(cpu);
    state.ran.store(passed ? 1 : 2, std::memory_order_release);
    sched::exit_current();
}
} // namespace

// A committed operation owns its completion through suspension and resource recall.
struct ExecutionWait {
    Op operation{};
    Cpus& cpus;
    bool overrun{};
    std::atomic<bool> entered{}, returned{};
};
static void execution_entry(void* p) noexcept {
    auto& op = *static_cast<ExecutionWait*>(p);
    auto& thread = *current_cpu().current;
    sync::Irq irq;
    thread.enter_kernel();
    if (op.overrun) {
        auto& clock = current_cpu().dispatcher().clock();
        const auto until = clock.rd_ticks() + *clock.ticks<2, time::ms>();
        while (clock.rd_ticks() < until) asm volatile("" ::: "memory");
    }
    libk_assert(op.operation.events().await({thread, op.cpus, current_cpu().dispatcher()},
        {.mask = OP_DONE}).status == STATUS_PENDING);
    op.entered.store(true, std::memory_order_release);
    thread.block();
    thread.leave_kernel();
    current_cpu().dispatcher().resched(); // The production return boundary.
    op.returned.store(true, std::memory_order_release);
}
static auto execution_lifetime(Cpu& runtime) noexcept -> bool {
    auto& k = *test::boot;
    auto& clock = k.clock;
    auto wait = [&](auto predicate) noexcept {
        const auto until = clock.rd_ticks() + clock.rd_freq();
        while (!predicate() && clock.rd_ticks() < until) sched::yield();
        libk_assert(predicate());
    };
    auto create = [&](ExecutionWait& op, CpuId cpu) noexcept {
        auto stack = mm::Stack::create(k.vm);
        libk_assert(stack);
        auto pending = k.objects.get<Thread>().create(std::move(*stack), Env::kernel(k.vm),
            Thread::KernelStart{execution_entry, &op});
        libk_assert(pending);
        auto thread = pending->publish();
        wait([&] { return k.cpus.get(cpu)->online(); });
        libk_assert(thread->claim_home(k.cpus.get(cpu)->dispatcher()));
        return thread;
    };
    for (bool stop : {false, true}) {
        ExecutionWait op{.cpus = k.cpus, .overrun = !stop};
        op.operation.accept(); libk_assert(op.operation.commit());
        auto thread = create(op, k.cpus.count() > 1 ? CpuId{1} : runtime.id);
        auto pending = k.objects.get<sched::Sc>().create(sched::Sc::Config{
            .budget = stop ? 0 : *clock.ticks<1, time::ms>(),
            .period = stop ? 0 : *clock.ticks<100, time::ms>()}, clock.rd_ticks());
        libk_assert(pending);
        auto sc = pending->publish();
        libk_assert(k.domain->admit(sc.get(), thread->home_cpu()));
        libk_assert(sc->bind(sc, thread.get()) && thread->execution(true));
        wait([&] { return op.entered.load(std::memory_order_acquire); });
        if (!stop) libk_assert(thread->execution(false));
        const auto generation = sc->generation();
        if (stop) libk_assert(sc.destroy());
        else libk_assert(sc->withdraw(generation));
        wait([&] { return !sc->info(clock.rd_ticks()).bound; });
        libk_assert(!thread->stop_requested());
        op.operation.finish();
        const auto delay = clock.rd_ticks() + *clock.ticks<5, time::ms>();
        while (clock.rd_ticks() < delay) sched::yield();
        libk_assert(!op.returned.load(std::memory_order_acquire));
        if (stop) {
            thread->stop();
            wait([&] { return thread->stopped(); });
            libk_assert(!op.returned.load(std::memory_order_acquire));
        } else {
            const auto debt = sc->info(clock.rd_ticks()).debt;
            libk_assert(debt != 0);
            ExecutionWait other{.cpus = k.cpus};
            auto borrower = create(other, runtime.id);
            libk_assert(sc->bind(sc, borrower.get()));
            const auto changed = sc->info(clock.rd_ticks());
            libk_assert(changed.bound && changed.debt == debt);
            libk_assert(sc->withdraw(changed.generation));
            wait([&] { return !sc->info(clock.rd_ticks()).bound; });
            borrower->stop();
            wait([&] { return borrower->stopped(); });
            libk_assert(borrower.destroy());
            libk_assert(sc->bind(sc, thread.get()));
            libk_assert(thread->detach_resource(generation) == STATUS_STALE); // Old association cannot recall the new one.
            libk_assert(thread->execution(true));
            wait([&] { return thread->stopped(); });
            libk_assert(op.returned.load(std::memory_order_acquire));
            libk_assert(sc.destroy());
        }
        libk_assert(thread.destroy());
    }
    console::print<"[scenario] execution lifetime ok: no-SC completion, resume, recall and maintenance Stop\n">();
    return true;
}

static void fair_entry(void* p) noexcept {
    auto& done = *static_cast<std::atomic<bool>*>(p);
    while (!done.load(std::memory_order_acquire)) asm volatile("" ::: "memory");
}
static auto fair_execution(Cpu&) noexcept -> bool {
    auto& k = *test::boot;
    if (k.cpus.count() < 2) return true;
    const CpuId cpu{1};
    auto& clock = k.clock;
    std::array<std::atomic<bool>, 3> done{};
    std::array<object::ref<Thread>, 3> threads;
    std::array<object::ref<sched::Sc>, 3> resources;
    for (usize i = 0; i < threads.size(); ++i) {
        auto stack = mm::Stack::create(k.vm);
        libk_assert(stack);
        auto thread = k.objects.get<Thread>().create(std::move(*stack), Env::kernel(k.vm),
            Thread::KernelStart{fair_entry, &done[i]});
        libk_assert(thread);
        threads[i] = thread->publish();
        libk_assert(threads[i]->claim_home(k.cpus.get(cpu)->dispatcher()));
        const sched::Sc::Config config{.budget = i == 2 ? *clock.ticks<1, time::ms>() : 0,
            .period = i == 2 ? *clock.ticks<10, time::ms>() : 0, .weight = i == 1 ? 2048U : 1024U};
        auto sc = k.objects.get<sched::Sc>().create(config, clock.rd_ticks());
        libk_assert(sc);
        resources[i] = sc->publish();
        libk_assert(k.domain->admit(resources[i].get(), cpu));
        libk_assert(resources[i]->bind(resources[i], threads[i].get()) && threads[i]->execution(true));
    }
    const auto started = clock.rd_ticks();
    while (clock.rd_ticks() - started < *clock.ticks<300, time::ms>()) sched::yield();
    std::array<u64, 3> used;
    for (usize i = 0; i < used.size(); ++i) used[i] = threads[i]->info(clock.rd_ticks()).runtime;
    const auto elapsed = clock.rd_ticks() - started;
    libk_assert(used[0] && used[1] * 10 > used[0] * 14 && used[1] * 10 < used[0] * 26);
    libk_assert(used[2] * 100 < elapsed * 15);
    done[1].store(true, std::memory_order_release); done[2].store(true, std::memory_order_release);
    while (!threads[1]->stopped() || !threads[2]->stopped()) sched::yield();
    const auto before = threads[0]->info(clock.rd_ticks()).runtime;
    const auto alone = clock.rd_ticks();
    while (clock.rd_ticks() - alone < *clock.ticks<50, time::ms>()) sched::yield();
    const auto spare = threads[0]->info(clock.rd_ticks()).runtime - before;
    libk_assert(spare * 10 > (clock.rd_ticks() - alone) * 7);
    done[0].store(true, std::memory_order_release);
    while (!threads[0]->stopped()) sched::yield();
    for (usize i = 0; i < threads.size(); ++i) libk_assert(threads[i].destroy() && resources[i].destroy());
    console::print<"[scenario] fair execution ok: weighted={}/{}/{} cap=10% idle capacity consumed\n">(used[0], used[1], used[2]);
    return true;
}

struct OrdinaryWork {
    std::atomic<unsigned> steps{};
    Work work{Work::Fn::bind<&OrdinaryWork::step>(*this), Work::Fn::bind<&OrdinaryWork::cancel>(*this)};
    void step() noexcept {
        const auto end = test::boot->clock.rd_ticks() + *test::boot->clock.ticks<100, time::us>();
        while (test::boot->clock.rd_ticks() < end) {}
        steps.fetch_add(1, std::memory_order_release);
    }
    void cancel() noexcept {}
};
static constinit libk::manual_lifetime<std::array<OrdinaryWork, 8>> ordinary_storage;

static auto domain_execution(Cpu&) noexcept -> bool {
    auto& k = *test::boot;
    if (k.cpus.count() < 4) return true;
    auto& clock = k.clock;
    const auto mask = (u64{1} << k.cpus.count()) - 1;
    std::array<object::ref<sched::Domain>, 4> domains;
    for (usize i = 0; i < domains.size(); ++i) {
        const sched::Domain::Config config{.cpus=mask,
            .budget=i == 2 ? *clock.ticks<1, time::ms>() : 0,
            .period=i == 2 ? *clock.ticks<10, time::ms>() : 0};
        auto made = k.objects.get<sched::Domain>().create(k.pmm, k.cpus, config);
        libk_assert(made && made->get().open() == STATUS_OK);
        domains[i] = made->publish();
        libk_assert(domains[i]->attach(i == 3 ? domains[2] : k.domain) == STATUS_OK);
    }
    std::array<std::atomic<bool>, 6> done{};
    std::array<object::ref<Thread>, 6> threads;
    std::array<object::ref<sched::Sc>, 6> contexts;
    for (usize i = 0; i < threads.size(); ++i) {
        const CpuId cpu{i < 4 ? 1 : i - 2};
        auto stack = mm::Stack::create(k.vm);
        libk_assert(stack);
        auto t = k.objects.get<Thread>().create(std::move(*stack), Env::kernel(k.vm),
            Thread::KernelStart{fair_entry, &done[i]});
        libk_assert(t);
        threads[i] = t->publish();
        libk_assert(threads[i]->claim_home(k.cpus.get(cpu)->dispatcher()));
        auto sc = k.objects.get<sched::Sc>().create(sched::Sc::Config{}, clock.rd_ticks());
        libk_assert(sc);
        contexts[i] = sc->publish();
        libk_assert(domains[i < 3 ? 0 : i == 3 ? 1 : 3]->admit(contexts[i].get(), cpu));
        libk_assert(contexts[i]->bind(contexts[i], threads[i].get()) && threads[i]->execution(true));
    }
    const auto started = clock.rd_ticks();
    while (clock.rd_ticks() - started < *clock.ticks<300, time::ms>()) sched::yield();
    std::array<u64, 6> used;
    for (usize i = 0; i < used.size(); ++i) used[i] = threads[i]->info(clock.rd_ticks()).runtime;
    const auto elapsed = clock.rd_ticks() - started;
    const auto many = used[0] + used[1] + used[2];
    libk_assert(many && used[3] && many * 10 > used[3] * 7 && many * 10 < used[3] * 14);

    libk_assert((used[4] + used[5]) && (used[4] + used[5]) * 100 < elapsed * 16);
    const auto config = sched::Domain::Config{.cpus=mask, .budget=*clock.ticks<2, time::ms>(),
        .period=*clock.ticks<20, time::ms>(), .weight=512};
    const auto sequence = domains[2]->configure(config, &k.domain.get());
    libk_assert(sequence);
    while (domains[2]->info(clock.rd_ticks()).applied != *sequence) sched::yield();
    libk_assert(domains[2]->info(clock.rd_ticks()).status == STATUS_OK);
    for (auto& flag : done) flag.store(true, std::memory_order_release);
    for (auto& t : threads) while (!t->stopped()) sched::yield();
    for (usize i = 0; i < threads.size(); ++i) libk_assert(threads[i].destroy() && contexts[i].destroy());
    auto& jobs = ordinary_storage.emplace();
    for (auto& work : jobs) libk_assert(domains[3]->post(work.work) == STATUS_OK);
    for (auto& work : jobs) while (!work.steps.load(std::memory_order_acquire)) sched::yield();
    // Closing the ancestor must drain its real child index and every home.
    for (usize i : {0U, 1U, 2U}) static_cast<void>(domains[i]->close());
    for (auto& d : domains) {
        while (d->info(clock.rd_ticks()).phase != DOMAIN_DRAINED) sched::yield();
        libk_assert(d->info(clock.rd_ticks()).work_runtime || &d == &domains[0] || &d == &domains[1]);
        libk_assert(d.destroy());
    }
    ordinary_storage.reset();
    console::print<"[scenario] domain execution ok: siblings={}/{} aggregate={}/{} home recall drained\n">(many, used[3], used[4] + used[5], elapsed);
    return true;
}

auto wait_publication(Cpu& runtime) noexcept -> bool {
    libk_assert(source_history(runtime));
    libk_assert(execution_lifetime(runtime));
    libk_assert(fair_execution(runtime));
    libk_assert(domain_execution(runtime));
    if (test::boot == nullptr ||
        runtime.cpus == nullptr) {
        return false;
    }
    Boot& kernel = *test::boot;
    auto stack = mm::Stack::create(kernel.vm);
    if (!stack) {
        return false;
    }
    WaitState state{};
    auto pending_thread =
        kernel.objects.get<Thread>().create(std::move(stack).value(), Env::kernel(kernel.vm),
                                      Thread::KernelStart{wait_entry, &state});
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
        sched::Sc::Config{.budget = *budget, .period = *period}, kernel.clock.rd_ticks());
    if (!pending_context) {
        static_cast<void>(thread.destroy());
        thread.reset();

        return false;
    }
    auto context = std::move(pending_context).value().publish();
    const CpuId cpu = runtime.id;
    auto admitted = kernel.domain.get().admit(context.get(), cpu);
    auto target = thread;
    if (!admitted || !target || !thread->claim_home(runtime.dispatcher()) || !context->bind(context, thread.get())) {
        if (admitted) {
            static_cast<void>(kernel.domain.get().unadmit(context.get()));
        }
        static_cast<void>(context.destroy());
        context.reset();
        static_cast<void>(thread.destroy());
        thread.reset();

        return false;
    }

    const bool started = thread->execution(true).has_value();
    if (started) {
        // Yield is a scheduling opportunity, not a join. Wait for the real
        // terminal state before unbinding and releasing the test's stack.
        while (thread->state() != Thread::State::Exited) sched::yield();
    }
    // Exited precedes the dispatcher's release of its binding. Keep both
    // objects alive until the asynchronous unbind has actually detached it.
    bool context_unbound = !context->bound();
    while (!context_unbound) {
        context_unbound = !context->bound();
        if (!context_unbound) sched::yield();
    }
    while (context->bound()) sched::yield();
    const bool unadmitted = static_cast<bool>(kernel.domain.get().unadmit(context.get()));
    const bool context_retired = context.destroy();
    const bool thread_retired = thread.destroy();
    context.reset();
    thread.reset();

    const bool result = started && state.ran.load(std::memory_order_acquire) == 1 && context_unbound &&
                        unadmitted && context_retired && thread_retired;
    if (result) console::print<"[scenario] wait publication ok\n">();
    return result;
}

} // namespace test::scenario
