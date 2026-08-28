#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)

completion="$repo_root/kernel/operation/completion.cpp"
completion_h="$repo_root/kernel/include/operation/completion.hpp"
wait_source="$repo_root/kernel/operation/wait.cpp"
wait_h="$repo_root/kernel/include/operation/wait.hpp"
vproc="$repo_root/kernel/execution/vproc.cpp"
endpoint="$repo_root/kernel/ipc/endpoint.cpp"
trap_source="$repo_root/kernel/trap/trap.cpp"
notification="$repo_root/kernel/ipc/notification.cpp"
notification_h="$repo_root/kernel/include/ipc/notification.hpp"

fail() {
    printf '%s\n' "[audit] FAIL: $*" >&2
    exit 1
}

require() {
    file=$1
    pattern=$2
    description=$3
    rg -q -- "$pattern" "$file" || fail "$description"
}

reject_if() {
    file=$1
    pattern=$2
    description=$3
    if rg -q -- "$pattern" "$file"; then
        fail "$description"
    fi
}

check_contract() {
    c=$1
    ch=$2
    w=$3
    wh=$4
    v=$5
    e=$6
    t=$7

    require "$ch" 'try_claim_finish' 'Completion finish claim is missing'
    require "$ch" 'try_claim_cancel' 'Completion cancel claim is missing'
    require "$ch" 'resolve_cancel' 'Completion cancel resolution is missing'
    require "$ch" 'try_reopen_cancel' 'Completion reopen proof is missing'
    require "$ch" 'finish_claimed' 'Completion finish terminal is missing'
    require "$ch" 'finalize_cancel' 'Completion cancel terminal is missing'
    reject_if "$ch" 'auto finish\(' 'old callback-bearing Completion::finish API remains'
    reject_if "$ch" 'auto cancel\(' 'old callback-bearing Completion::cancel API remains'

    require "$c" 'sched::PreemptGuard preempt' 'BlockingSink PreemptGuard is missing'
    require "$c" 'delivery_\.store<libk::MemoryOrder::Release>\(Delivery::Ready\)' \
        'Ready release publication is missing'
    [ "$(rg -c 'delivery_\.store<libk::MemoryOrder::Release>\(Delivery::Detached\)' "$c")" -eq 4 ] \
        || fail 'terminal Delivery publication count changed'
    [ "$(rg -c 'observation_key_\.exchange<libk::MemoryOrder::AcqRel>\(0\)' "$c")" -ge 3 ] \
        || fail 'terminal paths do not retire their observation generation'
    [ "$(rg -c 'ops->release\(owner\)' "$c")" -eq 4 ] \
        || fail 'terminal owner release count changed'
    reject_if "$c" 'ops_->release\(' \
        'terminal path calls release through a live Completion member'
    detached_lines=$(rg -n \
        'delivery_\.store<libk::MemoryOrder::Release>\(Delivery::Detached\)' \
        "$c" | cut -d: -f1)
    for detached_line in $detached_lines; do
        prior_key=$(sed -n "1,${detached_line}p" "$c" \
            | rg -n 'observation_key_\.exchange<libk::MemoryOrder::AcqRel>\(0\)' \
            | tail -1 | cut -d: -f1)
        [ -n "$prior_key" ] && [ "$prior_key" -lt "$detached_line" ] \
            || fail 'Detached publication precedes terminal key retirement'
        next_detached=$(printf '%s\n' "$detached_lines" \
            | awk -v line="$detached_line" '$1 > line {print $1; exit}')
        if [ -n "$next_detached" ]; then
            terminal_end=$((next_detached - 1))
        else
            terminal_end=$(wc -l < "$c")
        fi
        release_line=$(sed -n "$((detached_line + 1)),${terminal_end}p" "$c" \
            | rg -n 'ops->release\(owner\)' | head -1 | cut -d: -f1)
        [ -n "$release_line" ] \
            || fail 'Detached publication has no captured owner release'
        if sed -n "$((detached_line + 1)),$((detached_line + release_line - 1))p" "$c" \
            | rg -q '(sink_|delivery_|observation_key_)'; then
            fail 'Completion member access remains between Detached and release'
        fi
    done
    reject_if "$c" 'while \(expected == Delivery::Claimed\)' \
        'old spinning Completion finish path remains'
    guard_line=$(rg -n 'sched::PreemptGuard preempt' "$c" | cut -d: -f1)
    wake_line=$(rg -n 'wait->wake\(\)' "$c" | cut -d: -f1 | head -1)
    ready_line=$(rg -n 'delivery_\.store<libk::MemoryOrder::Release>\(Delivery::Ready\)' "$c" | cut -d: -f1 | head -1)
    [ -n "$guard_line" ] && [ -n "$wake_line" ] && [ -n "$ready_line" ] \
        || fail 'cannot locate BlockingSink publication boundaries'
    [ "$guard_line" -lt "$wake_line" ] \
        || fail 'PreemptGuard does not cover Wait::wake'
    [ "$wake_line" -lt "$ready_line" ] \
        || fail 'Ready release precedes Wait::wake'
    [ "$(rg -c 'sched::PreemptGuard preempt' "$c")" -eq 1 ] \
        || fail 'PreemptGuard was broadened beyond BlockingSink'

    require "$wh" 'EdgePhase' 'Wait borrowed-edge phase is missing'
    require "$wh" 'FinishOwned' 'Wait finish ownership phase is missing'
    require "$w" 'completion->try_claim_finish\(\)' \
        'Wait finish does not claim Delivery under its lock'
    [ "$(rg -c 'phase_ = EdgePhase::FinishOwned;' "$w")" -ge 2 ] \
        || fail 'Wait does not publish FinishOwned for both finish claims'
    require "$w" 'completion->try_claim_cancel\(\)' \
        'Wait cancel does not claim Delivery under its lock'
    require "$w" 'completion->finish_claimed\(trap\)' \
        'Wait finish terminal is missing'
    require "$w" 'completion->finalize_cancel\(resolution\)' \
        'Wait cancel terminal is missing'
    reject_if "$w" 'completion->(finish|cancel)\(' \
        'Wait retains the old unpinned Completion terminal call'
    require "$w" 'ready_\.store<libk::MemoryOrder::Release>\(true\)' \
        'Wait readiness is not published in wake'
    require "$w" 'sched::wake\(' 'Wait wake does not call scheduler after unlock'

    require "$v" 'completion->try_claim_cancel\(\)' \
        'Vproc cancellation does not claim Delivery under state_lock_'
    [ "$(rg -c 'completion->try_claim_cancel\(\)' "$v")" -ge 2 ] \
        || fail 'Vproc cancellation claim is missing from one operation path'
    require "$v" 'completion->resolve_cancel\(\)' \
        'Vproc cancellation resolution is missing'
    require "$v" 'clear_operation_locked' \
        'Vproc operation projection clear is missing'
    require "$v" 'completion->finalize_cancel\(resolution\)' \
        'Vproc cancellation terminal is missing'
    require "$v" 'slot\.generation == key\.generation\(\)' \
        'Vproc cancellation lacks exact generation validation'

    reject_if "$e" 'if \(wait\.attached\(\)\)' \
        'Endpoint publish_cancel retains attached() authority precheck'
    require "$e" 'static_cast<void>\(wait\.cancel\(\)\)' \
        'Endpoint publish_cancel does not call atomic Wait cancellation'
    require "$t" 'wait->ready\(\) && wait->finish\(context\)' \
        'trap exit does not recheck a failed finish claim'

    require "$notification_h" 'Done' \
        'Notification terminal owner state is missing'
    require "$notification" 'void Notification::release_wait\(\)' \
        'Notification release-side admission callback is missing'
    require "$notification" 'owner_->release_wait\(\)' \
        'Notification Wait release does not own admission reopening'
    require "$notification" \
        'wait_\.state_\.store<libk::MemoryOrder::Release>\(Wait::State::Done\)' \
        'Notification finish does not retire its generation before release'
    arm_line=$(rg -n '^auto Notification::Wait::arm' "$notification" \
        | cut -d: -f1)
    ready_method_line=$(rg -n '^auto Notification::Wait::ready' "$notification" \
        | cut -d: -f1)
    arm_body=$(sed -n "$arm_line,$((ready_method_line - 1))p" "$notification")
    printf '%s\n' "$arm_body" \
        | rg -q 'expected == State::Ready' \
        || fail 'Notification arm lost Ready terminal observation'
    printf '%s\n' "$arm_body" \
        | rg -q 'expected == State::Done' \
        || fail 'Notification arm lost Done terminal observation'
    printf '%s\n' "$arm_body" \
        | rg -q 'expected == State::Idle' \
        || fail 'Notification arm does not accept completed Idle cancellation'
    finish_line=$(rg -n '^auto Notification::finish_wait' "$notification" \
        | cut -d: -f1)
    release_wait_line=$(rg -n '^void Notification::release_wait' "$notification" \
        | cut -d: -f1)
    cancel_line=$(rg -n '^auto Notification::cancel_wait' "$notification" \
        | cut -d: -f1)
    retire_line=$(rg -n '^void Notification::retire' "$notification" \
        | cut -d: -f1)
    [ "$finish_line" -lt "$release_wait_line" ] \
        || fail 'Notification release callback is not after finish path'
    finish_body=$(sed -n "$finish_line,$((release_wait_line - 1))p" "$notification")
    printf '%s\n' "$finish_body" \
        | rg -q 'wait_\.state_\.store<libk::MemoryOrder::Release>\(Wait::State::Done\)' \
        || fail 'Notification finish does not retain a terminal owner state'
    cancel_body=$(sed -n "$cancel_line,$((retire_line - 1))p" "$notification")
    printf '%s\n' "$cancel_body" \
        | rg -q 'compare_exchange_weak' \
        || fail 'Notification cancellation lost its atomic state transition'
    printf '%s\n' "$cancel_body" \
        | rg -q 'observed, Wait::State::Done' \
        || fail 'Notification cancellation does not retain a terminal owner state'
    if sed -n "$((finish_line + 1)),$((release_wait_line - 1))p" "$notification" \
        | rg -q 'wait_\.state_\.store<libk::MemoryOrder::Release>\(Wait::State::Idle\)|cleanup_|Life::Closed'; then
        fail 'Notification finish path reopens or cleans up before release'
    fi
    if sed -n "$((cancel_line + 1)),$((retire_line - 1))p" "$notification" \
        | rg -q 'Wait::State::Idle'; then
        fail 'Notification cancellation reopens Idle before Completion release'
    fi
    release_body=$(sed -n "$release_wait_line,$((cancel_line - 1))p" "$notification")
    printf '%s\n' "$release_body" \
        | rg -q 'wait_\.state_\.store<libk::MemoryOrder::Release>\(Wait::State::Idle\)' \
        || fail 'Notification release callback does not reopen Idle'
    printf '%s\n' "$release_body" \
        | rg -q 'cleanup = libk::move\(cleanup_\)' \
        || fail 'Notification cleanup admission did not move to release'
}

check_contract \
    "$completion" "$completion_h" "$wait_source" "$wait_h" "$vproc" \
    "$endpoint" "$trap_source"
printf '%s\n' '[audit] operation Completion contract: OK'
