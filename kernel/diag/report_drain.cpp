#include <diag/owner.hpp>
#include <diag/concurrency_private.hpp>

#include <arch/interrupt.hpp>
#include <cpu/cpu_registry.hpp>
#include <cpu/cpu_runtime.hpp>
#include <diag/console.hpp>

namespace kernel::diag::concurrency {

auto drain_reports(CpuRegistry& registry) noexcept -> usize {
#if MYOS_CONCURRENCY_DIAG >= 3
    // Report output is deliberately restricted to a normal kernel-thread
    // context. The watchdog producer is IRQ-off and never calls this path.
    if (!arch::interrupts_enabled()) {
        return 0;
    }
    usize drained{};
    for (usize index = 0; index < registry.count(); ++index) {
        const CpuRuntime* const runtime = registry.runtime(CpuId{index});
        if (runtime == nullptr || runtime->diagnostics == nullptr) {
            continue;
        }
        auto& queue = runtime->diagnostics->concurrency->reports;
        ReportRecord report{};
        while (queue.consume(report)) {
            ++drained;
            diag::console::print<
                "[concurrency] report watcher={} target={} "
                "root-kind={} root={:#x} state={} class={} evidence={} "
                "age={} nodes={} truncated={} pending={}/{} omitted={}\n">(
                report.watcher.raw,
                report.target.raw,
                static_cast<u32>(report.root.kind),
                report.root.identity,
                static_cast<u32>(report.state),
                stall_class_name(report.classification),
                static_cast<u32>(report.evidence),
                report.age,
                report.graph.count,
                report.graph.truncated,
                report.graph.pending_shown,
                report.graph.pending_total,
                report.graph.pending_omitted);
            if (report.root_snapshot_valid) {
                const auto& snapshot = report.root_snapshot;
                diag::console::print<
                    "  root-snapshot gen={} kind={} phase={} wait={} "
                    "expectation={} subject={:#x} subject-gen={} "
                    "activity={} progress={} started={} last-activity={} "
                    "last-progress={} evidence={} semantic={}\n">(
                    snapshot.generation,
                    static_cast<u32>(snapshot.record_kind),
                    snapshot.phase,
                    static_cast<u32>(snapshot.wait_kind),
                    static_cast<u32>(snapshot.expectation),
                    snapshot.subject_identity,
                    snapshot.subject_generation,
                    snapshot.activity_epoch,
                    snapshot.progress_epoch,
                    snapshot.started_at,
                    snapshot.last_activity_at,
                    snapshot.last_progress_at,
                    static_cast<u32>(snapshot.evidence),
                    snapshot.semantic_stamp);
                diag::console::print<
                    "  root-relation wait-key={:#x} driver-kind={} "
                    "driver={:#x} driver-gen={} blocker-kind={} "
                    "blocker={:#x} blocker-gen={} site={}:{} function={} "
                    "detail={:#x},{:#x},{:#x},{:#x}\n">(
                    snapshot.wait_target.raw,
                    static_cast<u32>(snapshot.driver.kind),
                    snapshot.driver.identity,
                    snapshot.driver.generation,
                    static_cast<u32>(snapshot.blocker.kind),
                    snapshot.blocker.identity,
                    snapshot.blocker.generation,
                    snapshot.site.file,
                    snapshot.site.line,
                    snapshot.site.function,
                    snapshot.detail[0],
                    snapshot.detail[1],
                    snapshot.detail[2],
                    snapshot.detail[3]);
                diag::console::print<
                    "  root-policy kind={} expectation={} action={} "
                    "driver-kind={} driver={:#x} driver-gen={} "
                    "deadline-driver-kind={} deadline-driver={:#x} "
                    "deadline-driver-gen={} deadline={} grace={}\n">(
                    static_cast<u32>(snapshot.policy.kind),
                    static_cast<u32>(snapshot.policy.expectation),
                    static_cast<u32>(snapshot.policy.action),
                    static_cast<u32>(snapshot.policy.driver.kind),
                    snapshot.policy.driver.identity,
                    snapshot.policy.driver.generation,
                    static_cast<u32>(snapshot.policy.deadline_driver.kind),
                    snapshot.policy.deadline_driver.identity,
                    snapshot.policy.deadline_driver.generation,
                    snapshot.policy.deadline,
                    snapshot.policy.grace);
            } else {
                diag::console::print<"  root-snapshot unavailable\n">();
            }
            for (usize node = 0; node < report.graph.count; ++node) {
                const NodeRef value = report.graph.node(node);
                diag::console::print<
                    "  graph[{}] parent={} edge={} kind={} node={:#x} "
                    "gen={}\n">(
                    node,
                    report.graph.parents[node],
                    static_cast<u32>(report.graph.edge(node)),
                    static_cast<u32>(value.kind),
                    value.identity,
                    value.generation);
            }
        }
    }
    return drained;
#else
    static_cast<void>(registry);
    return 0;
#endif
}

} // namespace kernel::diag::concurrency
