#!/bin/sh
set -eu

source=$1
repo_root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
project_tmp="$repo_root/.tmp/project"
mkdir -p "$project_tmp"
class=$(sed -n '/class UartLoader final {/,/^};/p' "$source")
helpers=$(printf '%s\n' "$class" | sed -n '/template<typename Lease>/,/myos::cap::CapRef root_vspace_/p')

printf '%s\n' "$class" | rg -q '\[\[nodiscard\]\] auto cleanup\(\) noexcept -> bool'
printf '%s\n' "$class" | rg -U -q '(?s)close_lease\(scratch_\).*close_lease\(bundle_view_\).*close_task\(\);'
printf '%s\n' "$helpers" | rg -q 'for \(;;\)'
printf '%s\n' "$helpers" | rg -q 'retryable\(status\)'
printf '%s\n' "$helpers" | rg -q 'myos::yield\(\)'
if printf '%s\n' "$helpers" | rg -q 'attempt < 4|attempt\+\+'; then
    printf '%s\n' '[audit] FAIL: UART cleanup retains bounded silent retry' >&2
    exit 1
fi
if [ "$(printf '%s\n' "$helpers" | rg -c 'Backend::ownership_fault\(status\)')" -lt 2 ]; then
    printf '%s\n' '[audit] FAIL: UART cleanup lacks fail-stop on both owners' >&2
    exit 1
fi
rg -q 'if \(!uart\.cleanup\(\)\)' "$source"

audit_publish() {
    source=$1
    class=$(sed -n '/class UartLoader final {/,/^};/p' "$source")
    run_start=$(printf '%s\n' "$class" | rg -n '^    \[\[nodiscard\]\] auto run\(\)' | head -1 | cut -d: -f1 || true)
    prepare_start=$(printf '%s\n' "$class" | rg -n '^    \[\[nodiscard\]\] auto prepare\(' | head -1 | cut -d: -f1 || true)
    publish_start=$(printf '%s\n' "$class" | rg -n '^    \[\[nodiscard\]\] auto publish\(' | head -1 | cut -d: -f1 || true)
    helper_start=$(printf '%s\n' "$class" | rg -n '^    template<typename Lease>' | head -1 | cut -d: -f1 || true)
    [ -n "$run_start" ] && [ -n "$prepare_start" ] || return 1
    [ -n "$publish_start" ] && [ -n "$helper_start" ] || return 1

    run_block=$(printf '%s\n' "$class" | sed -n "${run_start},$((prepare_start - 1))p")
    retire_line=$(printf '%s\n' "$run_block" | rg -n 'materializer\.retire_sources\(image_\)' | head -1 | cut -d: -f1 || true)
    clear_line=$(printf '%s\n' "$run_block" | rg -n 'image_\.clear\(\)' | head -1 | cut -d: -f1 || true)
    publish_line=$(printf '%s\n' "$run_block" | rg -n 'if \(!publish\(\)\)' | head -1 | cut -d: -f1 || true)
    slot_line=$(printf '%s\n' "$run_block" | rg -n 'thread_slot_ = prepared_thread\.value\(\)' | head -1 | cut -d: -f1 || true)
    [ -n "$retire_line" ] && [ -n "$clear_line" ] && [ -n "$publish_line" ] || return 1
    [ -n "$slot_line" ] && [ "$slot_line" -lt "$retire_line" ] || return 1
    [ "$retire_line" -lt "$clear_line" ] && [ "$clear_line" -lt "$publish_line" ] || return 1

    prepare_block=$(printf '%s\n' "$class" | sed -n "${prepare_start},$((publish_start - 1))p")
    printf '%s\n' "$prepare_block" | rg -q 'thread_create\(' || return 1
    printf '%s\n' "$prepare_block" | rg -q 'adopt_local\(' || return 1
    printf '%s\n' "$prepare_block" | rg -q 'close_slot\(start_slot_\)' || return 1
    printf '%s\n' "$prepare_block" | rg -q 'sc_create\(' || return 1
    printf '%s\n' "$prepare_block" | rg -q 'sc_bind\(' || return 1
    printf '%s\n' "$prepare_block" | rg -q 'return thread_slot' || return 1
    if printf '%s\n' "$prepare_block" | rg -q 'execution_start\('; then
        return 1
    fi

    publish_block=$(printf '%s\n' "$class" | sed -n "${publish_start},$((helper_start - 1))p")
    printf '%s\n' "$publish_block" | rg -q 'task_\.lookup\(' || return 1
    printf '%s\n' "$publish_block" | rg -q 'thread_slot_' || return 1
    printf '%s\n' "$publish_block" | rg -q 'MYOS_OBJECT_KIND_THREAD' || return 1
    printf '%s\n' "$publish_block" | rg -q 'execution_start\(' || return 1
}

if ! audit_publish "$source"; then
    printf '%s\n' '[audit] FAIL: UART publication/order invariants' >&2
    exit 1
fi

mutation_dir=$(mktemp -d "$project_tmp/audit-uart-cleanup.XXXXXX")
trap 'rm -rf "$mutation_dir"' EXIT HUP INT TERM
expect_reject() {
    label=$1
    shift
    mutated="$mutation_dir/$label.cpp"
    sed "$@" "$source" > "$mutated"
    if audit_publish "$mutated" >/dev/null 2>&1; then
        printf '%s\n' "[audit] FAIL: UART mutation accepted: $label" >&2
        exit 1
    fi
    printf '%s\n' "[audit] UART mutation rejected: $label"
}

expect_reject start-before-retire \
    -e '/materializer\.retire_sources(image_)/d'
expect_reject missing-post-retire-lookup \
    -e '/task_\.lookup(/d'

printf '%s\n' '[audit] OK: UART cleanup retains armed owners until OK or ownership fault'
