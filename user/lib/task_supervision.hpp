#pragma once

/*
 * State-free supervision operations shared by init and process_server.
 * TaskRecord/TaskSpace remain the only owners of lifecycle and capability
 * state; these functions merely compose their existing checked operations.
 * Server-specific policy (which task to deploy, what to print, and which
 * authority identities to use) stays in the caller.
 */

#include <stddef.h>
#include <stdint.h>

#include <libk/optional.hpp>
#include <user/lib/deployment.hpp>
#include <user/lib/deployment_plan.hpp>
#include <user/lib/syscall.hpp>
#include <user/lib/task_transaction.hpp>
#include <uapi/status.h>

namespace myos::deploy::supervision {

template<typename Bundle, typename Plans>
[[nodiscard]] auto decode_plan(
    Bundle& bundle,
    ManifestWorkspace& manifest_workspace,
    Plans& plans,
    DeploymentPlan& plan,
    uint32_t expected_tasks) noexcept -> bool {
    const boot::Bundle* const package = bundle.view();
    if (package == nullptr) {
        return false;
    }
    boot::Module manifest{};
    if (!package->find("manifest", manifest) || !manifest.data_module()) {
        return false;
    }
    const boot::Bytes bytes = manifest.data();
    auto parsed = ManifestView::parse(
        bytes.data(), bytes.size(), manifest_workspace);
    if (!parsed) {
        return false;
    }
    auto view = parsed.value();
    if (!view.validate_boot_bundle(*package, manifest_workspace)) {
        return false;
    }
    auto decoded = plans.decode(view);
    if (!decoded) {
        return false;
    }
    plan = libk::move(decoded.value());
    return plan.task_count() == expected_tasks;
}

template<typename Table>
[[nodiscard]] auto drain(Table& table, TaskId id) noexcept -> bool {
    for (;;) {
        const myos_status_t status = table.continue_close(id);
        if (status == MYOS_STATUS_OK) {
            return true;
        }
        if (!myos::deploy::retryable(status)) {
            return false;
        }
        myos::yield();
    }
}

template<typename Table, typename Receiver>
[[nodiscard]] auto take_completion(
    Table& table,
    TaskId id,
    CloseReason reason,
    myos_status_t status,
    libk::optional<Receiver>& receiver) noexcept -> bool {
    if (!receiver || !receiver->valid() || !drain(table, id)) {
        return false;
    }
    const auto result = receiver->take();
    return result.has_value()
        && result->task == id
        && result->reason == reason
        && result->status == status;
}

template<typename Table, typename Receiver>
[[nodiscard]] auto observe_and_close(
    Table& table,
    TaskId id,
    libk::optional<Receiver>& receiver,
    myos_status_t& terminal_status) noexcept -> bool {
    terminal_status = MYOS_STATUS_INTERNAL;
    for (;;) {
        /* TaskTable owns the terminal relation selector.  The checked narrow
         * operation keeps supervision from recovering readiness or export
         * selectors through a public TaskRecord view. */
        const auto notification = table.terminal_notification(id);
        if (!notification) {
            return false;
        }
        const myos::SysResult wake = myos::notification_wait(
            notification->selector);
        if (wake.status == MYOS_STATUS_RETRY) {
            myos::yield();
            continue;
        }
        if (wake.status != MYOS_STATUS_OK) {
            return false;
        }

        const myos::SysResult observation = table.observe_terminal(id);
        if (observation.status != MYOS_STATUS_OK) {
            return false;
        }
        if (observation.value == 0) {
            myos::yield();
            continue;
        }
        terminal_status = static_cast<myos_status_t>(
            static_cast<int64_t>(observation.value2));
        const myos_status_t consumed = table.consume_terminal(
            id, observation);
        if (consumed == MYOS_STATUS_RETRY) {
            continue;
        }
        if (consumed != MYOS_STATUS_OK) {
            return false;
        }
        break;
    }

    if (!table.begin_close(id, CloseReason::Terminal, terminal_status)) {
        return false;
    }
    return take_completion(
        table, id, CloseReason::Terminal, terminal_status, receiver);
}

template<typename Table, typename Builder, typename Receiver>
[[nodiscard]] auto close_failed(
    Table& table,
    TaskId id,
    myos_status_t status,
    Builder& builder,
    libk::optional<Receiver>& receiver) noexcept -> bool {
    if (builder.valid()
        && !builder.fail(CloseReason::ConstructionFailure, status)) {
        return false;
    }
    return take_completion(
        table, id, CloseReason::ConstructionFailure, status, receiver);
}

} // namespace myos::deploy::supervision
