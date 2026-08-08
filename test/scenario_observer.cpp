#include <test/scenario.hpp>

#include <core/kernel_state.hpp>
#include <cpu/cpu_runtime.hpp>
#include <diag/concurrency.hpp>
#include <diag/console.hpp>
#include <libk/limits.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel::test::scenario::detail {
namespace {

struct Stats final {
    u64 iterations{};
    u64 total{};
    u64 minimum{libk::numeric_limits<u64>::max()};
    u64 maximum{};

    void add(u64 elapsed) noexcept {
        ++iterations;
        total += elapsed;
        if (elapsed < minimum) {
            minimum = elapsed;
        }
        if (elapsed > maximum) {
            maximum = elapsed;
        }
    }
};

} // namespace

auto observer(CpuRuntime& runtime) noexcept -> bool {
    if (runtime.kernel == nullptr || runtime.local.descriptor == nullptr) {
        return false;
    }

    const auto now = [&]() noexcept -> u64 {
        return runtime.kernel->clock().now().ticks();
    };

    Stats reserve_stats{};
    constexpr u64 reserve_iterations = 32;
    for (u64 index = 0; index < reserve_iterations; ++index) {
        const u64 started = now();
        auto observation = diag::concurrency::reserve(
            diag::concurrency::RecordKind::ServiceWork,
            runtime.local.descriptor->logical_id().raw,
            1,
            diag::concurrency::Expectation::ObserveOnly);
        if (observation) {
            observation.finish(static_cast<u32>(
                diag::concurrency::ServicePhase::Completed));
        }
        reserve_stats.add(now() - started);
    }

    Stats irq_stats{};
    constexpr u64 irq_iterations = 32;
    for (u64 index = 0; index < irq_iterations; ++index) {
        const u64 started = now();
        {
            sync::IrqToken irq{};
            static_cast<void>(irq.active());
        }
        irq_stats.add(now() - started);
    }

    Stats dispatch_stats{};
    constexpr u64 dispatch_iterations = 1;
    bool dispatch_ok = true;
    for (u64 index = 0; index < dispatch_iterations; ++index) {
        const u64 started = now();
        dispatch_ok = dispatch(runtime) && dispatch_ok;
        dispatch_stats.add(now() - started);
    }

    const bool result = dispatch_ok
        && reserve_stats.iterations == reserve_iterations
        && irq_stats.iterations == irq_iterations
        && dispatch_stats.iterations == dispatch_iterations;
    if (result) {
        diag::console::print<
            "[scenario] observer ok level={} "
            "reserve(n/min/total/max)={}/{}/{}/{} "
            "dispatch(n/min/total/max)={}/{}/{}/{} "
            "irq(n/min/total/max)={}/{}/{}/{}\n">(
            static_cast<u32>(diag::concurrency::level),
            reserve_stats.iterations,
            reserve_stats.minimum,
            reserve_stats.total,
            reserve_stats.maximum,
            dispatch_stats.iterations,
            dispatch_stats.minimum,
            dispatch_stats.total,
            dispatch_stats.maximum,
            irq_stats.iterations,
            irq_stats.minimum,
            irq_stats.total,
            irq_stats.maximum);
    }
    return result;
}

} // namespace kernel::test::scenario::detail
