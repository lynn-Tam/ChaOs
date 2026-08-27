#!/bin/sh
set -eu

source=$1
repo_root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
project_tmp="$repo_root/.tmp/project"
mkdir -p "$project_tmp"

audit_one() {
    source=$1

    line() {
        rg -n "$1" "$source" | head -1 | cut -d: -f1 || true
    }

    run_start=$(line 'auto run\(\) noexcept -> bool')
    make_start=$(line 'auto make_executions\(Materializer& materializer\)')
    retire_line=$(line 'materializer\.retire_sources\(image_\)')
    clear_line=$(line 'clear_image_sources\(\)')
    start_line=$(line 'if \(!start_executions\(\)\)')
    make_end=$(line 'auto start_executions\(\) noexcept -> bool')
    [ -n "$run_start" ] && [ -n "$make_start" ] || return 1
    [ -n "$retire_line" ] && [ -n "$clear_line" ] && [ -n "$start_line" ] || return 1
    [ -n "$make_end" ] || return 1
    [ "$run_start" -lt "$make_start" ] || return 1
    [ "$retire_line" -lt "$clear_line" ] || return 1
    [ "$clear_line" -lt "$start_line" ] || return 1

    make_block=$(sed -n "${make_start},$((make_end - 1))p" "$source")
    if printf '%s\n' "$make_block" | rg -q 'execution_start'; then
        return 1
    fi
    if printf '%s\n' "$make_block" | rg -q 'myos_cap_t targets'; then
        return 1
    fi
    printf '%s\n' "$make_block" | rg -q 'targets_\[' || return 1
    printf '%s\n' "$make_block" | rg -q 'target\.slot' || return 1

    start_block=$(sed -n "${make_end},/^    void observe_start/p" "$source")
    printf '%s\n' "$start_block" | rg -q 'task_\.lookup\(' || return 1
    printf '%s\n' "$start_block" | rg -q 'targets_\[index\]\.slot' || return 1
    printf '%s\n' "$start_block" | rg -q 'execution_start\(resolved\[index\]\.selector\)' || return 1

    descriptor_start=$(line 'auto construct_descriptor\(')
    descriptor_end=$(line 'auto make_executions\(Materializer& materializer\)')
    [ -n "$descriptor_start" ] && [ -n "$descriptor_end" ] || return 1
    descriptor_block=$(sed -n "${descriptor_start},$((descriptor_end - 1))p" "$source")
    adopt_rel=$(printf '%s\n' "$descriptor_block" \
        | rg -n 'adopt_local_selector\(result\.value, kind\)' \
        | tail -1 | cut -d: -f1 || true)
    close_rel=$(printf '%s\n' "$descriptor_block" \
        | rg -n 'close_slot\(slot\)' \
        | tail -1 | cut -d: -f1 || true)
    [ -n "$adopt_rel" ] && [ -n "$close_rel" ] && [ "$adopt_rel" -lt "$close_rel" ] || return 1
    printf '%s\n' "$descriptor_block" | rg -q 'output = produced\.value\(\)' || return 1

    thread_end=$(line 'for \(myos_word_t index = 0; index < thread_count_')
    [ -n "$thread_end" ] || return 1
    thread_block=$(sed -n "${make_start},$((thread_end - 1))p" "$source")
    printf '%s\n' "$thread_block" | rg -q 'construct_descriptor' || return 1
    [ "$(printf '%s\n' "$thread_block" | rg -c \
        'const auto context_slot = adopt_local_selector' || true)" -ge 1 ] || return 1
    printf '%s\n' "$thread_block" | rg -q 'sc_bind\(' || return 1
    [ "$(printf '%s\n' "$make_block" | rg -c \
        'const auto context_slot = adopt_local_selector' || true)" -ge 2 ] || return 1

    channel_start=$(line 'auto make_channel\(\) noexcept -> bool')
    channel_end=$(line 'auto make_vproc_runtime\(\) noexcept -> bool')
    [ -n "$channel_start" ] && [ -n "$channel_end" ] || return 1
    channel_block=$(sed -n "${channel_start},$((channel_end - 1))p" "$source")
    [ "$(printf '%s\n' "$channel_block" | rg -c 'channel_mint\(' || true)" -eq 3 ] || return 1
    [ "$(printf '%s\n' "$channel_block" | rg -c 'retain_remote\(' || true)" -ge 3 ] || return 1
    printf '%s\n' "$channel_block" | rg -q 'owner_type channel_a' || return 1
    printf '%s\n' "$channel_block" | rg -q 'owner_type channel_b' || return 1
    printf '%s\n' "$channel_block" | rg -q 'channel_a_slot' || return 1
    printf '%s\n' "$channel_block" | rg -q 'channel_b_slot' || return 1
    [ "$(printf '%s\n' "$channel_block" | rg -c \
        'const auto notify_[rs]_slot = adopt_local_selector' || true)" -eq 2 ] || return 1
    printf '%s\n' "$channel_block" | rg -q 'notify_r_slot' || return 1
    printf '%s\n' "$channel_block" | rg -q 'notify_s_slot' || return 1

    typed_start=$(line 'auto exercise_typed_delegate\(\) noexcept -> bool')
    typed_end=$(line 'auto make_pager\(\) noexcept -> bool')
    [ -n "$typed_start" ] && [ -n "$typed_end" ] || return 1
    typed_block=$(sed -n "${typed_start},$((typed_end - 1))p" "$source")
    preflight_line=$(printf '%s\n' "$typed_block" \
        | rg -n 'const auto manager = task_\.lookup\(' \
        | head -1 | cut -d: -f1 || true)
    delegate_line=$(printf '%s\n' "$typed_block" \
        | rg -n 'const auto result = myos::cap_typed_delegate\(' \
        | head -1 | cut -d: -f1 || true)
    owner_line=$(printf '%s\n' "$typed_block" \
        | rg -n 'typename Task::owner_type owner' \
        | head -1 | cut -d: -f1 || true)
    expected_line=$(printf '%s\n' "$typed_block" \
        | rg -n 'result\.status != MYOS_STATUS_OK' \
        | head -1 | cut -d: -f1 || true)
    close_line=$(printf '%s\n' "$typed_block" \
        | rg -n 'const myos_status_t closed = owner\.close\(\)' \
        | head -1 | cut -d: -f1 || true)
    fault_line=$(printf '%s\n' "$typed_block" \
        | rg -n 'Backend::ownership_fault\(closed\)' \
        | head -1 | cut -d: -f1 || true)
    adopt_line=$(printf '%s\n' "$typed_block" \
        | rg -n 'const auto adopted = task_\.adopt_remote_index\(' \
        | head -1 | cut -d: -f1 || true)
    [ -n "$preflight_line" ] && [ -n "$delegate_line" ] \
        && [ -n "$owner_line" ] && [ -n "$expected_line" ] \
        && [ -n "$close_line" ] && [ -n "$fault_line" ] \
        && [ -n "$adopt_line" ] || return 1
    [ "$preflight_line" -lt "$delegate_line" ] \
        && [ "$delegate_line" -lt "$owner_line" ] \
        && [ "$owner_line" -lt "$expected_line" ] \
        && [ "$expected_line" -lt "$close_line" ] \
        && [ "$close_line" -lt "$fault_line" ] \
        && [ "$fault_line" -lt "$adopt_line" ] || return 1
    guard_line=$(printf '%s\n' "$typed_block" \
        | rg -n 'if \(closed != MYOS_STATUS_OK\) \{' \
        | head -1 | cut -d: -f1 || true)
    [ -n "$guard_line" ] && [ "$guard_line" -eq "$((fault_line - 1))" ] \
        || return 1
    branch_end=$((adopt_line - 1))
    first_return_rel=$(printf '%s\n' "$typed_block" \
        | sed -n "${expected_line},${branch_end}p" \
        | rg -n 'return false;' \
        | head -1 | cut -d: -f1 || true)
    [ -n "$first_return_rel" ] || return 1
    first_return_line=$((expected_line + first_return_rel - 1))
    [ "$first_return_line" -gt "$fault_line" ] || return 1
    [ "$(printf '%s\n' "$typed_block" \
        | rg -c 'typed_call\(' || true)" -eq 4 ] || return 1
    [ "$(printf '%s\n' "$typed_block" \
        | rg -c 'myos::cap_typed_delegate\(' || true)" -eq 1 ] || return 1
    printf '%s\n' "$typed_block" | rg -q \
        'manager->selector != child_cspace_' || return 1
    printf '%s\n' "$typed_block" | rg -q \
        'task_\.can_adopt_remote\(\)' || return 1
    printf '%s\n' "$typed_block" | rg -q \
        'task_\.remote_live_size\(\) != before_remote' || return 1

    return 0
}

if ! audit_one "$source"; then
    printf '%s\n' '[audit] FAIL: proof publication/adoption invariants' >&2
    exit 1
fi

# Keep the gate non-vacuous: source mutations that remove one of the required
# ownership/order checks must be rejected by the same assertions.
mutation_dir=$(mktemp -d "$project_tmp/audit-proof-ownership.XXXXXX")
trap 'rm -rf "$mutation_dir"' EXIT HUP INT TERM
expect_reject() {
    label=$1
    shift
    mutated="$mutation_dir/$label.cpp"
    sed "$@" "$source" > "$mutated"
    if audit_one "$mutated" >/dev/null 2>&1; then
        printf '%s\n' "[audit] FAIL: mutation accepted: $label" >&2
        exit 1
    fi
    printf '%s\n' "[audit] mutation rejected: $label"
}

expect_reject missing-start -e '/if (!start_executions())/d'
expect_reject missing-start-lookup \
    -e '/targets_\[index\]\.slot/d'
expect_reject missing-descriptor-adoption \
    -e '/adopt_local_selector(result\.value, kind)/d'
expect_reject missing-sc-adoption \
    -e '/const auto context_slot = adopt_local_selector/d'
expect_reject missing-channel-result \
    -e '/const auto receiver = myos::channel_mint/,/if (!retain_remote(receiver, receiver_cap))/d'
expect_reject missing-notification-adoption \
    -e '/const auto notify_s_slot = adopt_local_selector/d'
expect_reject typed-missing-preflight \
    -e '/const auto manager = task_\.lookup/,+5d'
expect_reject typed-missing-owner \
    -e '/typename Task::owner_type owner/d'
expect_reject typed-missing-adoption \
    -e '/const auto adopted = task_\.adopt_remote_index/,+2d'
expect_reject typed-missing-site \
    -e '/if (!typed_call(/,+2d'
expect_reject typed-missing-exact-close \
    -e '/const myos_status_t closed = owner\.close\(\)/,+5d'
expect_reject typed-missing-close-fault \
    -e '/Backend::ownership_fault\(closed\)/d'
expect_reject typed-wrong-close-condition \
    -e 's/closed != MYOS_STATUS_OK/closed == MYOS_STATUS_OK/'
expect_reject typed-early-return \
    -e '/result\.status != MYOS_STATUS_OK/a\                return false;'

printf '%s\n' '[audit] OK: proof construction adopts every cap result before the next fallible action and starts only after source retirement'
