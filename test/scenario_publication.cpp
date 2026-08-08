#include <test/scenario.hpp>

#include <arch/time.hpp>
#include <core/kernel_state.hpp>
#include <cpu/cpu_registry.hpp>
#include <cpu/cpu_runtime.hpp>
#include <diag/concurrency.hpp>
#include <diag/console.hpp>
#include <libk/utility.hpp>
#include <mm/kernel_stack.hpp>
#include <object/resource_pool.hpp>
#include <resource/pool.hpp>
#include <sched/context.hpp>
#include <sched/dispatcher.hpp>
#include <sync/irq_lock_guard.hpp>
#include <thread/thread.hpp>

namespace kernel::test::scenario::detail {
namespace {

struct PoolWork final {
    object::ResourceHold pool{};
    resource::Permit permit{};
    libk::Atomic<u32> entered{};
    libk::Atomic<u32> release{};
    libk::Atomic<u32> done{};
};

void pool_entry(void* argument) noexcept {
    auto& work = *static_cast<PoolWork*>(argument);
    static_cast<void>(work.pool->close());
    work.entered.store<libk::MemoryOrder::Release>(1);
    while (work.release.load<libk::MemoryOrder::Acquire>() == 0) {
        // Re-enter the production close/service path while the permit keeps
        // the pool in Closing. Each publication advances activity without
        // semantic progress, which gives the watchdog its real livelock
        // classification witness rather than a synthetic counter update.
        static_cast<void>(work.pool->close());
        sched::yield();
    }
    work.permit.reset();
    work.done.store<libk::MemoryOrder::Release>(1);
    sched::exit_current();
}

[[nodiscard]] auto flight_has_watchdog(
    const CpuRuntime& runtime,
    u64 before,
    diag::concurrency::ObservationKey root) noexcept -> bool {
    const usize count = diag::concurrency::flight_count(runtime);
    diag::concurrency::FlightRecordValue value{};
    for (usize index = 0; index < count; ++index) {
        if (!diag::concurrency::flight_read(runtime, index, value)
            || value.absolute_id < before) {
            continue;
        }
        if (value.subject != root.raw) {
            continue;
        }
        if (value.event == diag::concurrency::FlightEvent::WatchdogConfirmed
            || value.event
                == diag::concurrency::FlightEvent::WatchdogLivelock) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] auto run_resource_watchdog(
    CpuRuntime& runtime,
    bool require_report) noexcept -> bool {
    if (runtime.kernel == nullptr || runtime.owner_registry == nullptr
        || runtime.local.descriptor == nullptr
        || !diag::concurrency::enabled(
            diag::concurrency::Level::Watch)) {
        return false;
    }
    KernelState& kernel = *runtime.kernel;
    CpuRegistry& cpus = *runtime.owner_registry;
    const CpuId boot = runtime.local.descriptor->logical_id();
    if (cpus.count() < 2) {
        return false;
    }
    const CpuId target_id{(boot.raw + 1) % cpus.count()};
    const CpuDescriptor* const target_descriptor = cpus.descriptor(target_id);
    CpuRuntime* const target_runtime = cpus.runtime(target_id);
    if (target_descriptor == nullptr || target_runtime == nullptr
        || target_descriptor->state() != CpuState::Online
        || target_runtime->diagnostics == nullptr) {
        return false;
    }

    auto pending_pool = kernel.objects().create_resource(
        resource::Budget{.memory = 1, .caps = 1});
    if (!pending_pool) {
        return false;
    }
    PoolWork work{};
    work.pool = libk::move(pending_pool).value().publish();
    auto pool_ref = work.pool.ref();
    if (!pool_ref) {
        return false;
    }
    auto pending_permit = work.pool->begin(libk::move(pool_ref).value());
    if (!pending_permit) {
        return false;
    }
    work.permit = libk::move(pending_permit).value();

    auto stack = KernelStack::create(kernel.kernel_vspace());
    if (!stack) {
        return false;
    }
    auto pending_thread = kernel.objects().create_thread(
        libk::move(stack).value(),
        ExecutionBinding::kernel(kernel.kernel_vspace()),
        Thread::KernelStart{pool_entry, &work});
    if (!pending_thread) {
        return false;
    }
    auto thread = libk::move(pending_thread).value().publish();
    const auto budget = kernel.clock().duration_from_nanoseconds(100'000'000);
    const auto period = kernel.clock().duration_from_nanoseconds(1'000'000'000);
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
    auto admitted = kernel.kernel_domain().admit(context.get(), target_id);
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
        && static_cast<bool>(sched::start(cpus, *binding));
    if (!started) {
        static_cast<void>(context->unbind());
        static_cast<void>(kernel.kernel_domain().unadmit(context.get()));
        static_cast<void>(context.retire());
        context.reset();
        static_cast<void>(thread.retire());
        thread.reset();
        kernel.objects().drain_reclaim();
        return false;
    }

    constexpr usize wait_spins = 1U << 22;
    for (usize spin = 0;
         spin < wait_spins
             && work.entered.load<libk::MemoryOrder::Acquire>() == 0;
         ++spin) {
        libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
    }
    if (work.entered.load<libk::MemoryOrder::Acquire>() == 0) {
        work.release.store<libk::MemoryOrder::Release>(1);
        for (usize spin = 0;
             spin < wait_spins
                 && work.done.load<libk::MemoryOrder::Acquire>() == 0;
             ++spin) {
            libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
        }
        return false;
    }

    // ResourcePool::close() publishes its observation before exposing
    // `entered`, but the target CPU may still be completing the watched-mask
    // release when the boot CPU resumes. Wait for that diagnostic projection
    // before driving the bounded watchdog sequence; this keeps the harness
    // witness about the production publication rather than a scheduler race.
    bool watched = diag::concurrency::observation_watched(*target_runtime) != 0;
    for (usize spin = 0; !watched && spin < wait_spins; ++spin) {
        watched = diag::concurrency::observation_watched(*target_runtime) != 0;
        libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
    }
    if (diag::concurrency::flight_head(runtime) == 0) {
        work.release.store<libk::MemoryOrder::Release>(1);
        return false;
    }
    diag::concurrency::ObservationKey pool_key{};
    for (usize index = 0;
         index < diag::concurrency::observation_slot_count();
         ++index) {
            const auto key = diag::concurrency::observation_key_at(
                *target_runtime, index);
            diag::concurrency::ObservationSnapshot snapshot{};
            if (!key || !diag::concurrency::observation_snapshot(
                    *target_runtime, key, snapshot)) {
                continue;
            }
            if (snapshot.record_kind
                    == diag::concurrency::RecordKind::ResourceClose
                && snapshot.subject_identity
                    == reinterpret_cast<u64>(&work.pool.get())) {
                pool_key = key;
                break;
            }
    }
    const u64 flight_before = diag::concurrency::flight_head(runtime);
    const u64 base = arch::read_clock().ticks();
    bool watchdog_event{};
    for (u32 pass = 1; pass <= 6; ++pass) {
        {
            sync::IrqToken irq{};
            diag::concurrency::watchdog_tick(
                boot,
                base + static_cast<u64>(pass) * 5'000'000'000ULL);
        }
        watchdog_event = watchdog_event
            || flight_has_watchdog(runtime, flight_before, pool_key);
    }
    const u32 reported_flag = static_cast<u32>(
        diag::concurrency::DiagnosticFlag::StallReported);
    watchdog_event = watchdog_event
        || (diag::concurrency::status_flags(*target_runtime)
                & reported_flag) != 0;

    bool report_consumed = !require_report;
    if (require_report) {
        const u32 reported = static_cast<u32>(
            diag::concurrency::DiagnosticFlag::StallReported);
        for (usize spin = 0; spin < wait_spins; ++spin) {
            const bool queued = diag::concurrency::report_pending(runtime);
            const bool marked = (
                diag::concurrency::status_flags(*target_runtime)
                    & reported) != 0;
            if (marked && !queued) {
                report_consumed = true;
                break;
            }
            libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
        }
    }

    work.release.store<libk::MemoryOrder::Release>(1);
    for (usize spin = 0;
         spin < wait_spins
             && work.done.load<libk::MemoryOrder::Acquire>() == 0;
         ++spin) {
        libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
    }
    const bool done = work.done.load<libk::MemoryOrder::Acquire>() != 0;
    for (usize spin = 0;
         spin < wait_spins && context->binding() != nullptr;
         ++spin) {
        libk::atomic_signal_fence<libk::MemoryOrder::SeqCst>();
    }
    const bool unbound = context->binding() == nullptr
        || static_cast<bool>(context->unbind());
    const bool unadmitted = static_cast<bool>(
        kernel.kernel_domain().unadmit(context.get()));
    const bool context_retired = context.retire();
    const bool thread_retired = thread.retire();
    context.reset();
    thread.reset();
    const bool pool_retired = work.pool.retire();
    work.pool.reset();
    kernel.objects().drain_reclaim();
    return done && watched && static_cast<bool>(pool_key) && watchdog_event
        && report_consumed && unbound && unadmitted && context_retired
        && thread_retired && pool_retired;
}

} // namespace

auto publication(CpuRuntime& runtime) noexcept -> bool {
    const bool result = run_resource_watchdog(runtime, false);
    if (result) {
        diag::console::print<"[scenario] publication ok\n">();
    }
    return result;
}

auto resource_watchdog(CpuRuntime& runtime, bool require_report) noexcept
    -> bool {
    return run_resource_watchdog(runtime, require_report);
}

} // namespace kernel::test::scenario::detail
