#pragma once

#include <libk/span.hpp>
#include <user/lib/deployment_plan.hpp>

namespace myos::deploy {

// Borrows one immutable plan. TaskTable alone owns execution state.
class ServiceGraph final {
    const DeploymentPlan& plan_;
public:
    explicit ServiceGraph(const DeploymentPlan& plan) noexcept : plan_(plan) {}

    // Optional dependencies do not gate startup or enlarge failure boundaries.
    auto order(libk::Span<uint32_t> output) const noexcept -> bool {
        const auto count = plan_.task_count();
        if (count == 0 || output.size() < count) return false;
        bool emitted[MYOS_DEPLOY_TASK_MAX]{};
        uint32_t written{};
        while (written < count) {
            bool progress{};
            for (uint32_t i = 0; i < count; ++i) {
                if (emitted[i]) continue;
                const auto& task = *plan_.task(i);
                bool blocked{};
                for (uint32_t d = 0; d < task.dependencies.count; ++d) {
                    const auto& edge = *plan_.dependency(task.dependencies.first + d);
                    if (edge.kind == MYOS_DEPLOY_DEPENDENCY_REQUIRED
                        && (edge.flags & (MYOS_DEPLOY_DEPENDENCY_STARTUP | MYOS_DEPLOY_DEPENDENCY_READINESS))
                        && !emitted[edge.target]) {
                        blocked = true;
                        break;
                    }
                }
                if (blocked) continue;
                emitted[i] = true;
                output[written++] = i;
                progress = true;
            }
            if (!progress) return false;
        }
        return true;
    }

    // Seed failed providers, then find transitive consumers. Reverse order()
    // supplies the corresponding consumer-before-provider teardown order.
    auto affected(libk::Span<bool> failed) const noexcept -> bool {
        if (failed.size() < plan_.task_count()) return false;
        bool changed;
        do {
            changed = false;
            for (uint32_t i = 0; i < plan_.task_count(); ++i) {
                if (failed[i]) continue;
                const auto& task = *plan_.task(i);
                for (uint32_t d = 0; d < task.dependencies.count; ++d) {
                    const auto& edge = *plan_.dependency(task.dependencies.first + d);
                    if (edge.kind == MYOS_DEPLOY_DEPENDENCY_REQUIRED && failed[edge.target]) {
                        failed[i] = true;
                        changed = true;
                        break;
                    }
                }
            }
        } while (changed);
        return true;
    }
};

} // namespace myos::deploy
