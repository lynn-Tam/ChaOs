#include <diag/owner.hpp>
#include <sync/trace.hpp>

#include <arch/cpu.hpp>
#include <arch/interrupt.hpp>
#include <arch/time.hpp>
#include <core/debug.hpp>
#include <cpu/cpu_local.hpp>
#include <cpu/cpu_registry.hpp>
#include <cpu/cpu_runtime.hpp>
#include <diag/panic.hpp>
#include <diag/console.hpp>
#include <libk/limits.hpp>
#include <mm/pmm.hpp>
#include <sync/model.hpp>
#include <sync/irq_lock_guard.hpp>
#include <trap/event.hpp>

namespace kernel::sync {

#ifndef MYOS_LOCK_DIAG
#define MYOS_LOCK_DIAG 0
#endif
#ifndef MYOS_CONCURRENCY_DIAG
#define MYOS_CONCURRENCY_DIAG 0
#endif

namespace {

consteval auto selected_sync_level() noexcept -> Level {
    if constexpr (MYOS_LOCK_DIAG >= 3 || MYOS_CONCURRENCY_DIAG >= 4) {
        return Level::Profile;
    } else if constexpr (MYOS_LOCK_DIAG >= 2 || MYOS_CONCURRENCY_DIAG >= 1) {
        return Level::Trace;
    } else if constexpr (MYOS_LOCK_DIAG >= 1) {
        return Level::Verify;
    } else {
        return Level::Off;
    }
}

} // namespace

extern const Level level = selected_sync_level();

auto enabled(Level required) noexcept -> bool {
    return static_cast<u8>(level) >= static_cast<u8>(required);
}

#if MYOS_LOCK_DIAG == 0 && MYOS_CONCURRENCY_DIAG == 0
auto provision(CpuRuntime& runtime, mm::Pmm& pmm) noexcept -> bool {
    static_cast<void>(runtime);
    static_cast<void>(pmm);
    return true;
}
#endif

#if MYOS_LOCK_DIAG >= 1 || MYOS_CONCURRENCY_DIAG >= 1
namespace {

constexpr bool lock_verify = MYOS_LOCK_DIAG >= 1;
constexpr bool lock_trace = MYOS_LOCK_DIAG >= 2
    || MYOS_CONCURRENCY_DIAG >= 1;
constexpr bool lock_profile = MYOS_LOCK_DIAG >= 3;

constexpr usize class_count = static_cast<usize>(LockClass::Count);
enum class Violation : u32 {
    Recursion = 1,
    HeldOverflow,
    SameClass,
    DependencyCycle,
    WrongOwner,
    NonLifo,
    HeldAtBoundary,
    IrqImbalance,
    WaitCycle,
    Threshold,
};

struct CurrentTrace final {
    CpuLockTrace* trace{};
    CpuId cpu{};
    CpuRegistry* registry{};
};

[[nodiscard]] auto current_trace() noexcept -> CurrentTrace {
    void* const owner = arch::current_cpu_owner();
    if (owner == nullptr) {
        return {};
    }
    auto& cpu = *static_cast<CpuLocal*>(owner);
    if (cpu.descriptor == nullptr || cpu.runtime_ == nullptr
        || cpu.runtime_->diagnostics == nullptr) {
        return {};
    }
    return CurrentTrace{
        &cpu.runtime_->diagnostics->locks,
        cpu.descriptor->logical_id(),
        cpu.runtime_->owner_registry,
    };
}

[[noreturn]] void fail(
    Violation violation,
    LockSite site,
    usize arg0 = 0,
    usize arg1 = 0,
    usize arg2 = 0) noexcept {
    diag::fatal(
        diag::FatalEvent{
            .facility = diag::Facility::Synchronization,
            .id = diag::EventId{0x90000000U
                + static_cast<u32>(violation)},
            .arguments = {arg0, arg1, arg2},
            .argument_count = 3,
        },
        diag::SourceLocation{site.file, site.function, nullptr, site.line});
}

[[nodiscard]] constexpr auto class_index(LockClass value) noexcept -> usize {
    return static_cast<usize>(value);
}

#if MYOS_LOCK_DIAG >= 3
void atomic_max(libk::Atomic<u64>& target, u64 value) noexcept {
    u64 current = target.load<libk::MemoryOrder::Relaxed>();
    while (current < value
        && !target.compare_exchange_weak<
            libk::MemoryOrder::Relaxed,
            libk::MemoryOrder::Relaxed>(current, value)) {}
}

void atomic_add_sat(libk::Atomic<u64>& target, u64 value) noexcept {
    u64 current = target.load<libk::MemoryOrder::Relaxed>();
    for (;;) {
        const u64 limit = libk::numeric_limits<u64>::max();
        const u64 next = value > limit - current ? limit : current + value;
        if (target.compare_exchange_weak<
                libk::MemoryOrder::Relaxed,
                libk::MemoryOrder::Relaxed>(current, next)) {
            return;
        }
    }
}
#endif

[[nodiscard]] auto now() noexcept -> u64 {
#if MYOS_LOCK_DIAG >= 3 || MYOS_CONCURRENCY_DIAG >= 1
    return arch::read_clock().ticks();
#else
    return 0;
#endif
}

void begin_record(libk::Atomic<u64>& sequence, u64& odd) noexcept {
    const u64 current = sequence.load<libk::MemoryOrder::Relaxed>();
    if (current >= libk::numeric_limits<u64>::max() - 2) {
        odd = 0;
        return;
    }
    odd = (current & 1U) == 0 ? current + 1 : current + 2;
    sequence.store<libk::MemoryOrder::Release>(odd);
}

void end_record(libk::Atomic<u64>& sequence, u64 odd) noexcept {
    if (odd != 0) {
        sequence.store<libk::MemoryOrder::Release>(odd + 1);
    }
}

void publish_wait(
    CpuLockTrace& trace, LockRef lock, LockSite site, bool active) noexcept {
    u64 odd{};
    begin_record(trace.waiting.sequence, odd);
    if (odd == 0) {
        trace.degraded.store<libk::MemoryOrder::Release>(1);
        return;
    }
    u64 generation = trace.wait_generation.load<libk::MemoryOrder::Relaxed>();
    if (generation == libk::numeric_limits<u64>::max()) {
        trace.degraded.store<libk::MemoryOrder::Release>(1);
        end_record(trace.waiting.sequence, odd);
        return;
    }
    if (active) {
        ++generation;
        trace.wait_generation.store<libk::MemoryOrder::Relaxed>(generation);
    }
    trace.waiting.address.store<libk::MemoryOrder::Relaxed>(
        active ? reinterpret_cast<usize>(lock.address) : 0);
    trace.waiting.owner.store<libk::MemoryOrder::Relaxed>(
        active ? lock.owner : nullptr);
    trace.waiting.lock_class.store<libk::MemoryOrder::Relaxed>(
        active ? static_cast<u32>(lock.lock_class) : 0);
    trace.waiting.line.store<libk::MemoryOrder::Relaxed>(active ? site.line : 0);
    trace.waiting.generation.store<libk::MemoryOrder::Relaxed>(generation);
    trace.waiting.wait_started.store<libk::MemoryOrder::Relaxed>(
        active ? now() : 0);
    trace.waiting.active.store<libk::MemoryOrder::Relaxed>(active);
    end_record(trace.waiting.sequence, odd);
    if (active) {
        const auto token = diag::concurrency::set_wait(
            diag::concurrency::WaitKind::SpinLock,
            {},
            diag::concurrency::NodeRef::external(
                reinterpret_cast<u64>(lock.address), generation),
            diag::concurrency::NodeRef::external(
                reinterpret_cast<u64>(lock.owner)),
            diag::concurrency::SourceSite{site.file, site.function, site.line});
        trace.waiting.concurrency_token.store<libk::MemoryOrder::Release>(
            token.raw);
    } else {
        const u64 token = trace.waiting.concurrency_token.exchange<
            libk::MemoryOrder::AcqRel>(0);
        diag::concurrency::clear_wait(
            diag::concurrency::WaitToken{token});
    }
}

void publish_held(CpuLockTrace& trace) noexcept {
    const usize count = trace.local.held_count;
    for (usize index = 0; index < remote_held_capacity; ++index) {
        RemoteHeld& remote = trace.held[index];
        u64 odd{};
        begin_record(remote.sequence, odd);
        if (odd == 0) {
            trace.degraded.store<libk::MemoryOrder::Release>(1);
            continue;
        }
        if (index < count) {
            const HeldEntry& held = trace.local.held[index];
            remote.address.store<libk::MemoryOrder::Relaxed>(
                reinterpret_cast<usize>(held.lock.address));
            remote.lock_class.store<libk::MemoryOrder::Relaxed>(
                static_cast<u32>(held.lock.lock_class));
            remote.line.store<libk::MemoryOrder::Relaxed>(held.site.line);
            remote.acquired_at.store<libk::MemoryOrder::Relaxed>(
                held.acquired_at);
        } else {
            remote.address.store<libk::MemoryOrder::Relaxed>(0);
            remote.lock_class.store<libk::MemoryOrder::Relaxed>(0);
            remote.line.store<libk::MemoryOrder::Relaxed>(0);
            remote.acquired_at.store<libk::MemoryOrder::Relaxed>(0);
        }
        end_record(remote.sequence, odd);
    }
    trace.held_count.store<libk::MemoryOrder::Release>(
        static_cast<u32>(count));
}

void publish_event(
    CpuLockTrace& trace,
    LockEvent event,
    LockRef lock,
    u32 extra,
    LockSite site) noexcept {
    static_cast<void>(trace);
    const auto translated = [&]() noexcept {
        switch (event) {
        case LockEvent::Acquire:
            return diag::concurrency::FlightEvent::LockAcquire;
        case LockEvent::Release:
            return diag::concurrency::FlightEvent::LockRelease;
        case LockEvent::Contended:
            return diag::concurrency::FlightEvent::LockContended;
        case LockEvent::IrqOff:
            return diag::concurrency::FlightEvent::IrqOff;
        case LockEvent::IrqOn:
            return diag::concurrency::FlightEvent::IrqOn;
        }
        return diag::concurrency::FlightEvent::ObservationDegraded;
    }();
    diag::concurrency::record(
        diag::concurrency::FlightDomain::Lock,
        translated,
        0,
        reinterpret_cast<u64>(lock.address),
        static_cast<u64>(lock.lock_class),
        extra,
        0,
        diag::concurrency::SourceSite{site.file, site.function, site.line});
}

struct EdgeWitness final {
    libk::Atomic<u64> sequence{};
    libk::Atomic<const char*> from_file{};
    libk::Atomic<const char*> from_function{};
    libk::Atomic<const char*> to_file{};
    libk::Atomic<const char*> to_function{};
    libk::Atomic<u32> from_line{};
    libk::Atomic<u32> to_line{};
};

struct GraphState final {
    static constexpr usize word_bits = sizeof(u64) * 8;
    static constexpr usize word_count =
        (class_count + word_bits - 1) / word_bits;

    libk::Atomic<u64> rows[class_count][word_count]{};
    EdgeWitness witnesses[class_count][class_count]{};

    [[nodiscard]] auto publish_witness(
        usize from,
        usize to,
        LockSite from_site,
        LockSite to_site) noexcept -> bool {
        EdgeWitness& witness = witnesses[from][to];
        u64 expected{};
        if (!witness.sequence.compare_exchange_strong<
                libk::MemoryOrder::AcqRel,
                libk::MemoryOrder::Acquire>(expected, 1)) {
            // A completed witness is immutable. If its first writer is still
            // active, that writer also owns the later adjacency publication.
            return (expected & 1U) == 0;
        }
        witness.from_file.store<libk::MemoryOrder::Relaxed>(from_site.file);
        witness.from_function.store<libk::MemoryOrder::Relaxed>(
            from_site.function);
        witness.to_file.store<libk::MemoryOrder::Relaxed>(to_site.file);
        witness.to_function.store<libk::MemoryOrder::Relaxed>(
            to_site.function);
        witness.from_line.store<libk::MemoryOrder::Relaxed>(from_site.line);
        witness.to_line.store<libk::MemoryOrder::Relaxed>(to_site.line);
        witness.sequence.store<libk::MemoryOrder::Release>(2);
        return true;
    }

    [[nodiscard]] auto read_witness(
        usize from,
        usize to,
        LockSite& from_site,
        LockSite& to_site) const noexcept -> bool {
        const EdgeWitness& witness = witnesses[from][to];
        for (usize attempt = 0; attempt < 3; ++attempt) {
            const u64 first =
                witness.sequence.load<libk::MemoryOrder::Acquire>();
            if ((first & 1U) != 0 || first == 0) {
                continue;
            }
            LockSite read_from{
                witness.from_file.load<libk::MemoryOrder::Relaxed>(),
                witness.from_function.load<libk::MemoryOrder::Relaxed>(),
                witness.from_line.load<libk::MemoryOrder::Relaxed>()};
            LockSite read_to{
                witness.to_file.load<libk::MemoryOrder::Relaxed>(),
                witness.to_function.load<libk::MemoryOrder::Relaxed>(),
                witness.to_line.load<libk::MemoryOrder::Relaxed>()};
            if (witness.sequence.load<libk::MemoryOrder::Acquire>() == first) {
                from_site = read_from;
                to_site = read_to;
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] auto check_insert(
        usize from,
        usize to,
        LockSite from_site,
        LockSite to_site) noexcept -> DepGraph<class_count>::Result {
        if (!publish_witness(from, to, from_site, to_site)) {
            return {DepStatus::Exists, {}};
        }
        const u64 mask = u64{1} << (to % word_bits);
        const u64 old = rows[from][to / word_bits]
            .fetch_or<libk::MemoryOrder::SeqCst>(mask);
        if ((old & mask) != 0) {
            return {DepStatus::Exists, {}};
        }

        DepGraph<class_count> graph{};
        for (usize row = 0; row < class_count; ++row) {
            for (usize word = 0; word < word_count; ++word) {
                const u64 bits =
                    rows[row][word].load<libk::MemoryOrder::SeqCst>();
                for (usize bit = 0; bit < word_bits; ++bit) {
                    if ((bits & (u64{1} << bit)) != 0) {
                        const usize target = word * word_bits + bit;
                        if (target < class_count) {
                            graph.insert(row, target);
                        }
                    }
                }
            }
        }
        DepGraph<class_count>::Path reverse{};
        if (graph.path(to, from, reverse)) {
            return {DepStatus::Cycle, reverse};
        }
        return {DepStatus::Added, {}};
    }

    [[nodiscard]] auto snapshot() const noexcept -> DepGraph<class_count> {
        DepGraph<class_count> result{};
        for (usize row = 0; row < class_count; ++row) {
            for (usize word = 0; word < word_count; ++word) {
                const u64 bits = rows[row][word].load<
                    libk::MemoryOrder::Acquire>();
                for (usize bit = 0; bit < word_bits; ++bit) {
                    if ((bits & (u64{1} << bit)) != 0) {
                        const usize target = word * word_bits + bit;
                        if (target < class_count) {
                            result.insert(row, target);
                        }
                    }
                }
            }
        }
        return result;
    }
};

constinit GraphState graph_state{};

struct GraphCycle final {
    bool found{};
    LockClass from{};
    LockClass to{};
    usize path_size{};
};

void publish_dependency(
    CpuLockTrace& trace,
    usize index,
    LockClass from,
    LockClass to,
    LockSite from_site,
    LockSite to_site) noexcept {
    DepLink& remote = trace.dep_cycle[index];
    u64 odd{};
    begin_record(remote.sequence, odd);
    if (odd == 0) {
        trace.degraded.store<libk::MemoryOrder::Release>(1);
        return;
    }
    remote.from_file.store<libk::MemoryOrder::Relaxed>(from_site.file);
    remote.to_file.store<libk::MemoryOrder::Relaxed>(to_site.file);
    remote.lines.store<libk::MemoryOrder::Relaxed>(
        static_cast<u64>(from_site.line)
            | (static_cast<u64>(to_site.line) << 32));
    remote.classes.store<libk::MemoryOrder::Relaxed>(
        static_cast<u32>(from) | (static_cast<u32>(to) << 8));
    end_record(remote.sequence, odd);
}

// Keep the bounded graph snapshot off the acquisition caller's frame. The
// kernel stack audit verifies this boundary in every enabled diagnostic mode.
[[gnu::noinline]]
[[nodiscard]] auto add_dependencies(
    CpuLockTrace& trace,
    const LocalLockState& local,
    LockClass target,
    LockSite target_site) noexcept -> GraphCycle {
    GraphCycle cycle{};
    const usize to = class_index(target);
    for (usize index = 0; index < local.held_count; ++index) {
        const HeldEntry& held = local.held[index];
        const usize from = class_index(held.lock.lock_class);
        if (from == to) {
            continue;
        }
        const auto result =
            graph_state.check_insert(from, to, held.site, target_site);
        if (result.status == DepStatus::Cycle) {
            cycle.found = true;
            cycle.from = held.lock.lock_class;
            cycle.to = target;
            cycle.path_size = result.path.size;
            usize step_count{};
            for (usize path_index = 1;
                path_index < result.path.size
                    && step_count + 1 < dep_cycle_capacity;
                ++path_index) {
                const usize edge_from = result.path.nodes[path_index - 1];
                const usize edge_to = result.path.nodes[path_index];
                LockSite from_site{};
                LockSite to_site{};
                static_cast<void>(graph_state.read_witness(
                    edge_from, edge_to, from_site, to_site));
                publish_dependency(
                    trace,
                    step_count++,
                    static_cast<LockClass>(edge_from),
                    static_cast<LockClass>(edge_to),
                    from_site,
                    to_site);
            }
            publish_dependency(
                trace,
                step_count++,
                held.lock.lock_class,
                target,
                held.site,
                target_site);
            trace.dep_cycle_size.store<libk::MemoryOrder::Release>(step_count);
            break;
        }
    }
    return cycle;
}

void validate_local(
    const LocalLockState& local,
    LockRef lock,
    LockSite site) noexcept {
    if (local.held_count == local_held_capacity) {
        fail(Violation::HeldOverflow, site, local.held_count);
    }
    for (usize index = 0; index < local.held_count; ++index) {
        const HeldEntry& held = local.held[index];
        if (held.lock.address == lock.address) {
            fail(Violation::Recursion, site,
                reinterpret_cast<usize>(lock.address));
        }
        if (held.lock.lock_class != lock.lock_class) {
            continue;
        }
        if (lock.same_class == SameClassPolicy::Forbidden
            || reinterpret_cast<usize>(lock.address)
                <= reinterpret_cast<usize>(held.lock.address)) {
            fail(Violation::SameClass, site,
                reinterpret_cast<usize>(held.lock.address),
                reinterpret_cast<usize>(lock.address),
                class_index(lock.lock_class));
        }
    }
}

void validate_graph(
    CpuLockTrace& trace,
    LockRef lock,
    LockSite site) noexcept {
    const GraphCycle cycle =
        add_dependencies(trace, trace.local, lock.lock_class, site);
    if (cycle.found) {
        fail(Violation::DependencyCycle, site,
            class_index(cycle.from), class_index(cycle.to), cycle.path_size);
    }
}

struct WaitSnapshot final {
    usize address{};
    libk::Atomic<u64>* owner{};
    u64 generation{};
    bool active{};
};

[[nodiscard]] auto read_wait(
    const RemoteWait& remote, WaitSnapshot& snapshot) noexcept -> bool {
    for (usize attempt = 0; attempt < 3; ++attempt) {
        const u64 first =
            remote.sequence.load<libk::MemoryOrder::Acquire>();
        if ((first & 1U) != 0) {
            continue;
        }
        WaitSnapshot value{
            remote.address.load<libk::MemoryOrder::Relaxed>(),
            remote.owner.load<libk::MemoryOrder::Relaxed>(),
            remote.generation.load<libk::MemoryOrder::Relaxed>(),
            remote.active.load<libk::MemoryOrder::Relaxed>(),
        };
        const u64 last = remote.sequence.load<libk::MemoryOrder::Acquire>();
        if (first == last && (last & 1U) == 0) {
            snapshot = value;
            return true;
        }
    }
    return false;
}

struct CycleStep final {
    CpuId cpu{};
    WaitSnapshot waiting{};
    u64 owner_word{};
};

[[nodiscard]] auto runtime_trace(
    CpuRegistry& registry, CpuId cpu) noexcept -> CpuLockTrace* {
    CpuRuntime* const runtime = registry.runtime(cpu);
    return runtime != nullptr && runtime->diagnostics != nullptr
        ? &runtime->diagnostics->locks
        : nullptr;
}

[[nodiscard]] auto stable_cycle(
    CurrentTrace current,
    CycleStep (&steps)[wait_cycle_capacity],
    usize& count) noexcept -> bool {
    if (current.registry == nullptr) {
        return false;
    }
    CpuId cursor = current.cpu;
    count = 0;
    for (; count < wait_cycle_capacity; ++count) {
        CpuLockTrace* const trace = runtime_trace(*current.registry, cursor);
        if (trace == nullptr
            || trace->degraded.load<libk::MemoryOrder::Acquire>() != 0) {
            return false;
        }
        WaitSnapshot waiting{};
        if (!read_wait(trace->waiting, waiting) || !waiting.active
            || waiting.owner == nullptr) {
            return false;
        }
        const u64 word =
            waiting.owner->load<libk::MemoryOrder::Acquire>();
        const usize next = OwnerWord::cpu(word);
        if (next >= max_cpu_count) {
            return false;
        }
        steps[count] = CycleStep{cursor, waiting, word};
        for (usize prior = 0; prior <= count; ++prior) {
            if (steps[prior].cpu.raw != next) {
                continue;
            }
            const usize cycle_begin = prior;
            WaitStamp candidate[wait_cycle_capacity]{};
            WaitStamp observed[wait_cycle_capacity]{};
            for (usize verify = cycle_begin; verify <= count; ++verify) {
                CpuLockTrace* const peer = runtime_trace(
                    *current.registry, steps[verify].cpu);
                WaitSnapshot current_wait{};
                if (peer == nullptr
                    || !read_wait(peer->waiting, current_wait)
                    || !current_wait.active || current_wait.owner == nullptr) {
                    return false;
                }
                const usize stamp = verify - cycle_begin;
                candidate[stamp] = WaitStamp{
                    steps[verify].cpu.raw,
                    steps[verify].waiting.address,
                    reinterpret_cast<usize>(steps[verify].waiting.owner),
                    steps[verify].waiting.generation,
                    steps[verify].owner_word};
                observed[stamp] = WaitStamp{
                    steps[verify].cpu.raw,
                    current_wait.address,
                    reinterpret_cast<usize>(current_wait.owner),
                    current_wait.generation,
                    current_wait.owner->load<libk::MemoryOrder::Acquire>()};
            }
            const usize cycle_count = count - cycle_begin + 1;
            if (validate_wait_cycle(
                    candidate, observed, cycle_count) != WaitCheck::Stable) {
                return false;
            }
            if (cycle_begin != 0) {
                for (usize move = cycle_begin; move <= count; ++move) {
                    steps[move - cycle_begin] = steps[move];
                }
                count -= cycle_begin;
            }
            ++count;
            return true;
        }
        cursor = CpuId{next};
    }
    return false;
}

void publish_cycle(
    CpuLockTrace& trace,
    const CycleStep (&steps)[wait_cycle_capacity],
    usize count) noexcept {
    const usize bounded = count < wait_cycle_capacity
        ? count : wait_cycle_capacity;
    for (usize index = 0; index < bounded; ++index) {
        WaitLink& target = trace.cycle[index];
        u64 odd{};
        begin_record(target.sequence, odd);
        if (odd == 0) {
            trace.degraded.store<libk::MemoryOrder::Release>(1);
            trace.cycle_size.store<libk::MemoryOrder::Release>(0);
            return;
        }
        target.lock.store<libk::MemoryOrder::Relaxed>(
            steps[index].waiting.address);
        target.owner_word.store<libk::MemoryOrder::Relaxed>(
            steps[index].owner_word);
        target.cpu.store<libk::MemoryOrder::Relaxed>(
            static_cast<u32>(steps[index].cpu.raw));
        end_record(target.sequence, odd);
    }
    trace.cycle_size.store<libk::MemoryOrder::Release>(bounded);
}

[[nodiscard]] auto context_for(const trap::Event& event) noexcept
    -> ExecContext {
    if (const auto* interrupt = event.interrupt()) {
        switch (interrupt->cause) {
        case trap::Interrupt::Timer:
            return ExecContext::TimerIrq;
        case trap::Interrupt::Software:
            return ExecContext::SoftwareIpi;
        case trap::Interrupt::External:
        case trap::Interrupt::Unknown:
            return ExecContext::ExternalIrq;
        }
    }
    if (event.origin() == trap::Origin::User) {
        const auto* exception = event.exception();
        return exception != nullptr && exception->cause == trap::Exception::Syscall
            ? ExecContext::UserSyscall
            : ExecContext::UserFault;
    }
    return ExecContext::KernelThread;
}

void push_context(CpuLockTrace& trace, ExecContext context) noexcept {
    if (trace.local.context_depth == context_capacity) {
        if constexpr (lock_verify) {
            fail(Violation::IrqImbalance, LockSite::current(), context_capacity);
        }
        trace.degraded.store<libk::MemoryOrder::Release>(1);
        return;
    }
    trace.local.contexts[trace.local.context_depth++] = context;
    trace.context.store<libk::MemoryOrder::Release>(
        static_cast<u32>(context));
}

void pop_context(CpuLockTrace& trace) noexcept {
    if (trace.local.context_depth <= 1) {
        if constexpr (lock_verify) {
            fail(Violation::IrqImbalance, LockSite::current());
        }
        trace.degraded.store<libk::MemoryOrder::Release>(1);
        return;
    }
    --trace.local.context_depth;
    trace.context.store<libk::MemoryOrder::Release>(static_cast<u32>(
        trace.local.contexts[trace.local.context_depth - 1]));
}

} // namespace

auto provision(CpuRuntime& runtime, mm::Pmm& pmm) noexcept -> bool {
#if MYOS_LOCK_DIAG == 0 && MYOS_CONCURRENCY_DIAG == 0
    static_cast<void>(runtime);
    static_cast<void>(pmm);
    return true;
#else
    if (runtime.diagnostics == nullptr) {
        return runtime.diagnostics != nullptr;
    }
#if MYOS_LOCK_DIAG >= 3
    auto* const trace = &runtime.diagnostics->locks;
    if (auto profile_page = pmm.allocate_page(); profile_page) {
        runtime.diagnostics->lock_profile_page =
            libk::move(profile_page).value();
        trace->profile = libk::construct_at(reinterpret_cast<LockProfile*>(
            runtime.diagnostics->lock_profile_page.bytes()));
    } else {
        trace->degraded.store<libk::MemoryOrder::Release>(1);
    }
#else
    static_cast<void>(pmm);
#endif
    return true;
#endif
}

auto before_acquire(LockRef lock, LockSite site) noexcept -> LockCookie {
    CurrentTrace current = current_trace();
    if (current.trace == nullptr) {
        return {};
    }
    if constexpr (lock_verify) {
        validate_local(current.trace->local, lock, site);
        validate_graph(*current.trace, lock, site);
    }
    if constexpr (lock_trace) {
        publish_wait(*current.trace, lock, site, true);
    }
    return LockCookie{true, lock_trace, now(), false};
}

auto after_acquire(LockRef lock, LockSite site, LockCookie cookie) noexcept
    -> LockCookie {
    if (!cookie.tracked) {
        return cookie;
    }
    CurrentTrace current = current_trace();
    if (current.trace == nullptr) {
        return {};
    }
    if constexpr (lock_trace) {
        publish_wait(*current.trace, lock, site, false);
        KASSERT(lock.owner != nullptr);
        const u64 old = lock.owner->load<libk::MemoryOrder::Acquire>();
        if (OwnerWord::cpu(old) != max_cpu_count) {
            if constexpr (lock_verify) {
                fail(Violation::WrongOwner, site,
                    reinterpret_cast<usize>(lock.address), old);
            }
            current.trace->degraded.store<libk::MemoryOrder::Release>(1);
            cookie.owner_tracked = false;
        }
        const u64 generation = OwnerWord::generation(old);
        if (generation == OwnerWord::max_generation) {
            current.trace->degraded.store<libk::MemoryOrder::Release>(1);
            cookie.owner_tracked = false;
        } else {
            lock.owner->store<libk::MemoryOrder::Release>(
                OwnerWord::pack(current.cpu, generation + 1));
        }
    }
    LocalLockState& local = current.trace->local;
    if (local.held_count >= local_held_capacity) {
        current.trace->degraded.store<libk::MemoryOrder::Release>(1);
        if constexpr (lock_verify) {
            fail(Violation::HeldOverflow, site, local.held_count);
        }
        return cookie;
    }
    local.held[local.held_count++] = HeldEntry{lock, site, now()};
    if constexpr (lock_trace) {
        publish_held(*current.trace);
        publish_event(*current.trace, LockEvent::Acquire, lock, 0, site);
    }
#if MYOS_LOCK_DIAG >= 3
    if (current.trace->profile != nullptr) {
        auto& stats =
            current.trace->profile->stats[class_index(lock.lock_class)];
        atomic_add_sat(stats.acquisitions, 1);
        static_cast<void>(stats.context_mask.fetch_or<
            libk::MemoryOrder::Relaxed>(
                u64{1} << current.trace->context.load<
                    libk::MemoryOrder::Relaxed>()));
        if (cookie.contended && cookie.wait_started != 0) {
            const u64 elapsed = now() - cookie.wait_started;
            atomic_add_sat(stats.wait_ticks, elapsed);
            atomic_max(stats.max_wait, elapsed);
        }
    }
#endif
    return cookie;
}

auto before_try(LockRef lock, LockSite site) noexcept -> LockCookie {
    CurrentTrace current = current_trace();
    if (current.trace == nullptr) {
        return {};
    }
    if constexpr (lock_verify) {
        validate_local(current.trace->local, lock, site);
    }
    return LockCookie{true, lock_trace, 0, false};
}

auto after_try(LockRef lock, LockSite site, LockCookie cookie) noexcept
    -> LockCookie {
    if (cookie.tracked) {
        CurrentTrace current = current_trace();
        if (current.trace != nullptr) {
            if constexpr (lock_verify) {
                validate_graph(*current.trace, lock, site);
            }
        }
    }
    return after_acquire(lock, site, cookie);
}

void cancel_try([[maybe_unused]] LockCookie cookie) noexcept {}

void before_release(LockRef lock, LockSite site, LockCookie cookie) noexcept {
    if (!cookie.tracked) {
        return;
    }
    CurrentTrace current = current_trace();
    if (current.trace == nullptr) {
        return;
    }
    LocalLockState& local = current.trace->local;
    if (local.held_count == 0
        || local.held[local.held_count - 1].lock.address != lock.address) {
        if constexpr (lock_verify) {
            fail(Violation::NonLifo, site,
                reinterpret_cast<usize>(lock.address), local.held_count);
        }
        current.trace->degraded.store<libk::MemoryOrder::Release>(1);
        return;
    }
#if MYOS_LOCK_DIAG >= 3
    const HeldEntry held = local.held[local.held_count - 1];
#endif
    if constexpr (lock_trace) {
        KASSERT(lock.owner != nullptr);
        const u64 word = lock.owner->load<libk::MemoryOrder::Acquire>();
        if (cookie.owner_tracked && OwnerWord::cpu(word) != current.cpu.raw) {
            if constexpr (lock_verify) {
                fail(Violation::WrongOwner, site,
                    reinterpret_cast<usize>(lock.address), word, current.cpu.raw);
            }
            current.trace->degraded.store<libk::MemoryOrder::Release>(1);
        }
        --local.held_count;
        publish_held(*current.trace);
        if (cookie.owner_tracked) {
            lock.owner->store<libk::MemoryOrder::Release>(
                OwnerWord::none(OwnerWord::generation(word)));
        }
        publish_event(*current.trace, LockEvent::Release, lock, 0, site);
#if MYOS_LOCK_DIAG >= 3
        if (held.acquired_at != 0 && current.trace->profile != nullptr) {
            auto& stats = current.trace->profile
                ->stats[class_index(lock.lock_class)];
            const u64 elapsed = now() - held.acquired_at;
            atomic_add_sat(stats.hold_ticks, elapsed);
            atomic_max(stats.max_hold, elapsed);
        }
#endif
    } else {
        --local.held_count;
    }
}

void on_spin(
    LockRef lock,
    LockSite site,
    [[maybe_unused]] u32 ticket,
    [[maybe_unused]] u32 serving,
    u32 polls) noexcept {
    CurrentTrace current = current_trace();
    if (current.trace == nullptr) {
        return;
    }
    if (polls == 1) {
        publish_event(*current.trace, LockEvent::Contended, lock, 0, site);
#if MYOS_LOCK_DIAG >= 3
        if (current.trace->profile != nullptr) {
            auto& stats =
                current.trace->profile->stats[class_index(lock.lock_class)];
            atomic_add_sat(stats.contentions, 1);
            atomic_max(stats.max_ticket_distance,
                static_cast<u32>(ticket - serving));
        }
#endif
    }
    CycleStep steps[wait_cycle_capacity]{};
    usize count{};
    if (stable_cycle(current, steps, count)) {
        publish_cycle(*current.trace, steps, count);
        if constexpr (lock_verify) {
            fail(Violation::WaitCycle, site, count,
                reinterpret_cast<usize>(lock.address));
        }
    }
}

auto irq_disabled(LockSite site) noexcept -> IrqCookie {
    CurrentTrace current = current_trace();
    if (current.trace == nullptr) {
        return {};
    }
    LocalLockState& local = current.trace->local;
    if (local.explicit_irq_depth == libk::numeric_limits<u16>::max()) {
        if constexpr (lock_verify) {
            fail(Violation::IrqImbalance, site, local.explicit_irq_depth);
        }
        current.trace->degraded.store<libk::MemoryOrder::Release>(1);
        return {};
    }
    if (local.explicit_irq_depth++ == 0) {
        local.explicit_irq_start = now();
        local.explicit_irq_site = site;
        if constexpr (lock_trace) {
            publish_event(*current.trace, LockEvent::IrqOff, {}, 0, site);
        }
    }
    return IrqCookie{current.cpu.raw, true};
}

void irq_restoring(IrqCookie cookie) noexcept {
    if (!cookie.tracked) {
        return;
    }
    CurrentTrace current = current_trace();
    if (current.trace == nullptr) {
        return;
    }
    if (cookie.cpu != current.cpu.raw) {
        if constexpr (lock_verify) {
            fail(Violation::IrqImbalance, LockSite::current(),
                cookie.cpu, current.cpu.raw);
        }
        current.trace->degraded.store<libk::MemoryOrder::Release>(1);
        return;
    }
    LocalLockState& local = current.trace->local;
    if (local.explicit_irq_depth == 0) {
        if constexpr (lock_verify) {
            fail(Violation::IrqImbalance, LockSite::current());
        }
        current.trace->degraded.store<libk::MemoryOrder::Release>(1);
        return;
    }
    if (--local.explicit_irq_depth == 0) {
#if MYOS_LOCK_DIAG >= 3
        if (local.explicit_irq_start != 0) {
            atomic_max(current.trace->explicit_irq_max,
                now() - local.explicit_irq_start);
        }
#endif
        if constexpr (lock_trace) {
            publish_event(*current.trace, LockEvent::IrqOn, {}, 0,
                LockSite::current());
        }
        local.explicit_irq_start = 0;
    }
}

void trap_enter(const trap::Event& event, u64 entry_tick) noexcept {
    CurrentTrace current = current_trace();
    if (current.trace == nullptr) {
        return;
    }
    push_context(*current.trace, context_for(event));
    [[maybe_unused]] const u32 depth =
        current.trace->hardware_irq_depth.fetch_add<
        libk::MemoryOrder::AcqRel>(1);
#if MYOS_LOCK_DIAG < 3
    static_cast<void>(entry_tick);
#endif
#if MYOS_LOCK_DIAG >= 3
    if (depth == 0) {
        current.trace->hardware_irq_start.store<libk::MemoryOrder::Release>(
            entry_tick);
    }
#endif
}

void trap_exiting() noexcept {
    CurrentTrace current = current_trace();
    if (current.trace == nullptr) {
        return;
    }
    auto& local = current.trace->local;
    if (local.context_depth == 0) {
        if constexpr (lock_verify) {
            fail(Violation::IrqImbalance, LockSite::current());
        }
        current.trace->degraded.store<libk::MemoryOrder::Release>(1);
        return;
    }
    local.contexts[local.context_depth - 1] = ExecContext::TrapExit;
    current.trace->context.store<libk::MemoryOrder::Release>(
        static_cast<u32>(ExecContext::TrapExit));
}

void trap_exit(u64 exit_tick) noexcept {
    CurrentTrace current = current_trace();
    if (current.trace == nullptr) {
        return;
    }
    const u32 old = current.trace->hardware_irq_depth.fetch_sub<
        libk::MemoryOrder::AcqRel>(1);
    if (old == 0) {
        if constexpr (lock_verify) {
            fail(Violation::IrqImbalance, LockSite::current());
        }
        current.trace->degraded.store<libk::MemoryOrder::Release>(1);
        return;
    }
#if MYOS_LOCK_DIAG < 3
    static_cast<void>(exit_tick);
#endif
#if MYOS_LOCK_DIAG >= 3
    {
        if (old == 1) {
            const u64 start = current.trace->hardware_irq_start.load<
                libk::MemoryOrder::Acquire>();
            if (exit_tick >= start) {
                atomic_max(current.trace->hardware_irq_max, exit_tick - start);
            }
        }
    }
#endif
    pop_context(*current.trace);
}

void panic_enter() noexcept {
    CurrentTrace current = current_trace();
    if (current.trace != nullptr) {
        current.trace->context.store<libk::MemoryOrder::Release>(
            static_cast<u32>(ExecContext::Panic));
    }
}

void assert_no_locks(LockSite site) noexcept {
    CurrentTrace current = current_trace();
    if (current.trace != nullptr && current.trace->local.held_count != 0) {
        fail(Violation::HeldAtBoundary, site,
            current.trace->local.held_count,
            reinterpret_cast<usize>(
                current.trace->local.held[
                    current.trace->local.held_count - 1].lock.address));
    }
}

void assert_held(LockRef lock, LockSite site) noexcept {
    CurrentTrace current = current_trace();
    if (current.trace == nullptr) {
        return;
    }
    for (usize index = 0; index < current.trace->local.held_count; ++index) {
        if (current.trace->local.held[index].lock.address == lock.address) {
            return;
        }
    }
    fail(Violation::WrongOwner, site,
        reinterpret_cast<usize>(lock.address));
}

auto lock_class_name(LockClass value) noexcept -> const char* {
    constexpr const char* names[] = {
        "pmm", "kernel-stack", "shootdown", "translation",
        "object-pool", "node-pool", "grant-graph", "grant-work",
        "cspace", "resource-pool", "vspace", "vspace-work",
        "memory-object", "backing-tree", "backing-storage",
        "physical-alias", "execution-authority", "sched-authority",
        "sched-context", "sched-domain", "remote-queue", "thread-stop",
        "vproc", "wait", "endpoint", "notification-source",
        "notification", "tunnel", "channel", "pager", "irq",
        "irq-registry", "terminal",
    };
    const usize index = class_index(value);
    return index < class_count ? names[index] : "invalid";
}

void dump_diagnostics() noexcept {
    if constexpr (!lock_trace) {
        return;
    }

    const DepGraph<class_count> graph = graph_state.snapshot();
    diag::console::print<"\n[sync] observed dependencies\n">();
    usize edges{};
    for (usize from = 0; from < class_count; ++from) {
        for (usize to = 0; to < class_count; ++to) {
            if (!graph.has(from, to)) {
                continue;
            }
            ++edges;
            diag::console::print<"  {} -> {}\n">(
                lock_class_name(static_cast<LockClass>(from)),
                lock_class_name(static_cast<LockClass>(to)));
        }
    }
    diag::console::print<"  edges={}\n">(edges);

#if MYOS_LOCK_DIAG >= 3
    {
        CurrentTrace current = current_trace();
        if (current.registry == nullptr) {
            return;
        }
        diag::console::print<"[sync] class profile\n">();
        for (usize index = 0; index < class_count; ++index) {
            u64 acquisitions{};
            u64 contentions{};
            u64 wait_ticks{};
            u64 hold_ticks{};
            u64 max_wait{};
            u64 max_hold{};
            u64 max_distance{};
            u64 contexts{};
            for (usize cpu = 0; cpu < current.registry->count(); ++cpu) {
                CpuLockTrace* const trace = runtime_trace(
                    *current.registry, CpuId{cpu});
                if (trace == nullptr) {
                    continue;
                }
                if (trace->profile == nullptr) {
                    continue;
                }
                const ClassStats& stats = trace->profile->stats[index];
                const auto add = [](u64 left, u64 right) noexcept {
                    const u64 limit = libk::numeric_limits<u64>::max();
                    return right > limit - left ? limit : left + right;
                };
                acquisitions = add(acquisitions,
                    stats.acquisitions.load<libk::MemoryOrder::Relaxed>());
                contentions = add(contentions,
                    stats.contentions.load<libk::MemoryOrder::Relaxed>());
                wait_ticks = add(wait_ticks,
                    stats.wait_ticks.load<libk::MemoryOrder::Relaxed>());
                hold_ticks = add(hold_ticks,
                    stats.hold_ticks.load<libk::MemoryOrder::Relaxed>());
                const u64 cpu_wait =
                    stats.max_wait.load<libk::MemoryOrder::Relaxed>();
                const u64 cpu_hold =
                    stats.max_hold.load<libk::MemoryOrder::Relaxed>();
                const u64 cpu_distance = stats.max_ticket_distance.load<
                    libk::MemoryOrder::Relaxed>();
                max_wait = cpu_wait > max_wait ? cpu_wait : max_wait;
                max_hold = cpu_hold > max_hold ? cpu_hold : max_hold;
                max_distance = cpu_distance > max_distance
                    ? cpu_distance : max_distance;
                contexts |=
                    stats.context_mask.load<libk::MemoryOrder::Relaxed>();
            }
            if (acquisitions == 0) {
                continue;
            }
            diag::console::print<
                "  {}: acq={} contended={} wait={} max-wait={} "
                "hold={} max-hold={} ticket={} contexts={:#x}\n">(
                lock_class_name(static_cast<LockClass>(index)),
                acquisitions, contentions, wait_ticks, max_wait,
                hold_ticks, max_hold, max_distance, contexts);
        }
    }
#endif
}

#else

auto before_acquire(LockRef, LockSite) noexcept -> LockCookie { return {}; }
auto after_acquire(LockRef, LockSite, LockCookie cookie) noexcept
    -> LockCookie { return cookie; }
auto before_try(LockRef, LockSite) noexcept -> LockCookie { return {}; }
auto after_try(LockRef, LockSite, LockCookie cookie) noexcept
    -> LockCookie { return cookie; }
void cancel_try(LockCookie) noexcept {}
void before_release(LockRef, LockSite, LockCookie) noexcept {}
void on_spin(LockRef, LockSite, u32, u32, u32) noexcept {}
auto irq_disabled(LockSite) noexcept -> IrqCookie { return {}; }
void irq_restoring(IrqCookie) noexcept {}
void trap_enter(const trap::Event&, u64) noexcept {}
void trap_exiting() noexcept {}
void trap_exit(u64) noexcept {}
void panic_enter() noexcept {}
void assert_no_locks(LockSite) noexcept {}
void assert_held(LockRef, LockSite) noexcept {}
auto lock_class_name(LockClass) noexcept -> const char* { return "off"; }
void dump_diagnostics() noexcept {}

#endif

} // namespace kernel::sync
