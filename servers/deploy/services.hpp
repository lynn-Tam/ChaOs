#pragma once

#include <optional>


#include <sys/clock.hpp>
#include <servers/deploy/launch.hpp>

namespace deploy {

// Policy worksets contain desired recovery, never a copy of TaskTable state.
// One graph owner serializes construction, observation and connection rebinding.
template<size_t Capacity, size_t Authorities = 16>
class services final {
    using Tasks = tasks<Capacity, Authorities>;
    Tasks& supervisor_;
    program& program_;
    sys::Clock clock_{};
    cap_t events_;
    std::optional<typename Tasks::handle> tasks_[Capacity];
    uint32_t order_[Capacity]{};
    bool recovering_[Capacity]{};
    bool disabled_[Capacity]{};
    bool needs_poll_{};
    uint64_t restart_at_{};
    // Rate-limit repeated crashes without spinning or replaying old requests.
    static constexpr uint64_t RestartDelayNs = 100'000'000;

    auto launch(uint32_t i) noexcept -> status_t {
        const auto& plan = program_.plan();
        const auto& row = *plan.task(i);
        const typename Tasks::handle* providers[Capacity]{};
        bool included[Capacity]{};
        size_t count{};
        for (uint32_t d = 0; d < row.dependencies.count; ++d) {
            const auto& edge = *plan.dependency(row.dependencies.first + d);
            if (tasks_[edge.target] && !included[edge.target]) {
                providers[count++] = &*tasks_[edge.target];
                included[edge.target] = true;
            }
        }
        status_t status{};
        tasks_[i] = supervisor_.launch(program_, plan.symbol(row.name), status,
            {.terminal_events = events_, .close_badge = word_t{1} << (i + 1)}, {providers, count});
        return status;
    }

    auto wait_ready(uint32_t i) noexcept -> status_t {
        const auto timeout = program_.plan().task(i)->readiness_timeout_ns;
        if (timeout == 0) return STATUS_OK;
        const auto deadline = clock_.after_ns(timeout);
        if (!deadline) return STATUS_BAD_ARGS;
        for (;;) {
            const auto observed = supervisor_.observe(*tasks_[i]);
            if (observed.status != STATUS_OK) return observed.status;
            if (observed.value != 0) {
                const auto status = static_cast<status_t>(static_cast<int64_t>(observed.value2));
                return status == STATUS_OK ? STATUS_PEER_FAULT : status;
            }
            const auto status = supervisor_.ready(*tasks_[i]);
            if (status == STATUS_OK) {
                const auto terminal = supervisor_.observe(*tasks_[i]);
                if (terminal.status != STATUS_OK) return terminal.status;
                if (terminal.value == 0) return STATUS_OK;
                const auto ended = static_cast<status_t>(static_cast<int64_t>(terminal.value2));
                return ended == STATUS_OK ? STATUS_PEER_FAULT : ended;
            }
            if (status != STATUS_WOULD_BLOCK) return status;
            const auto wake = sys::notification_wait(events_, *deadline);
            if (wake.status == STATUS_TIMED_OUT) return wake.status;
            if (wake.status != STATUS_OK) return wake.status;
            notify(wake.value);
        }
    }

public:
    struct start_result {
        status_t status;
        std::optional<uint32_t> task;
    };

    static_assert(Capacity < sizeof(word_t) * 8);
    services(Tasks& supervisor, program& program, cap_t events) noexcept
        : supervisor_(supervisor), program_(program), events_(events) {}

    auto start() noexcept -> start_result {
        const auto clock_status = clock_.open();
        if (clock_status != STATUS_OK) return {clock_status, std::nullopt};
        const auto& plan = program_.plan();
        if (plan.task_count() > Capacity || !plan.order(order_))
            return {STATUS_BAD_ARGS, std::nullopt};
        const auto valid = supervisor_.validate_graph(program_);
        if (valid != STATUS_OK) return {valid, std::nullopt};
        for (uint32_t p = 0; p < plan.task_count(); ++p) {
            auto status = launch(order_[p]);
            if (status == STATUS_OK) status = wait_ready(order_[p]);
            if (status == STATUS_OK) continue;
            // A dependent may own grants derived from its provider. Finish
            // each dependent's close before revoking the provider lineage.
            for (size_t q = p + 1; q != 0; --q) {
                auto& task = tasks_[order_[q - 1]];
                if (!task) continue;
                const auto stopped = supervisor_.request_stop(*task);
                if (stopped != STATUS_OK) return {stopped, order_[p]};
                for (;;) {
                    const auto result = supervisor_.collect(*task);
                    if (result.status == STATUS_OK) { task.reset(); break; }
                    if (!retryable(result.status) && result.status != STATUS_WOULD_BLOCK)
                        return {result.status, order_[p]};
                    if (supervisor_.closing_needs_poll(*task)) { sys::yield(); continue; }
                    const auto wake = sys::notification_wait(events_);
                    if (wake.status != STATUS_OK) return {wake.status, order_[p]};
                    // The notification is shared with all task generations.
                    notify(wake.value);
                }
            }
            return {status, order_[p]};
        }
        return {STATUS_OK, std::nullopt};
    }

    void notify(word_t badges) noexcept {
        for (auto& task : tasks_) if (task) supervisor_.notify(*task, badges);
    }
    auto needs_poll() const noexcept -> bool { return needs_poll_; }
    auto deadline() const noexcept -> uint64_t { return restart_at_; }
    auto restart(uint32_t index) noexcept -> status_t {
        if (index >= program_.plan().task_count() || !tasks_[index] || disabled_[index])
            return STATUS_NOT_FOUND;
        recovering_[index] = true;
        return STATUS_OK;
    }

    auto poll() noexcept -> status_t {
        const auto& plan = program_.plan();
        needs_poll_ = false;
        for (uint32_t i = 0; i < plan.task_count(); ++i) {
            if (!tasks_[i] || recovering_[i]) continue;
            const auto observed = supervisor_.observe(*tasks_[i]);
            if (observed.status != STATUS_OK) return observed.status;
            if (observed.value == 0) continue;
            recovering_[i] = true;
            const auto restart = clock_.after_ns(RestartDelayNs);
            if (!restart) return STATUS_BAD_ARGS;
            restart_at_ = *restart;
            const auto policy = plan.task(i)->restart;
            const auto status = static_cast<status_t>(observed.value2);
            if (policy == DEPLOY_RESTART_NEVER
                || (policy == DEPLOY_RESTART_ON_FAULT && status == STATUS_OK)) disabled_[i] = true;
        }
        (void)plan.affected(recovering_);
        (void)plan.affected(disabled_);
        for (size_t p = plan.task_count(); p != 0; --p) {
            const auto i = order_[p - 1];
            if (!recovering_[i] || !tasks_[i]) continue;
            // A provider's grants remain live until every dependent has
            // finished closing. Revoking both lineages concurrently conflicts
            // with a dependent's in-flight revocation of a derived grant.
            const auto stopped = supervisor_.request_stop(*tasks_[i]);
            if (stopped != STATUS_OK) return stopped;
            const auto result = supervisor_.collect(*tasks_[i]);
            if (result.status == STATUS_OK) tasks_[i].reset();
            else if (retryable(result.status) || result.status == STATUS_WOULD_BLOCK) {
                needs_poll_ = supervisor_.closing_needs_poll(*tasks_[i]);
                return STATUS_OK;
            } else return result.status;
        }
        if (restart_at_) {
            const auto now = sys::clock_now();
            if (now.status != STATUS_OK) return now.status;
            if (now.value < restart_at_) return STATUS_OK;
            restart_at_ = 0;
        }
        // No new instance borrows an old cohort's channels or memory. All
        // exact close completions were consumed before any relaunch begins.
        for (uint32_t p = 0; p < plan.task_count(); ++p) {
            const auto i = order_[p];
            if (!recovering_[i] || disabled_[i]) continue;
            auto status = launch(i);
            if (status == STATUS_OK) status = wait_ready(i);
            if (status != STATUS_OK) {
                // A partial new cohort must close before another attempt;
                // admission failure does not change the manifest's restart
                // policy or permanently disable healthy providers.
                const auto restart = clock_.after_ns(RestartDelayNs);
                if (!restart) return STATUS_BAD_ARGS;
                restart_at_ = *restart;
                needs_poll_ = true;
                return STATUS_OK;
            }
        }
        for (auto& recovery : recovering_) recovery = false;
        return STATUS_OK;
    }
};

} // namespace deploy
