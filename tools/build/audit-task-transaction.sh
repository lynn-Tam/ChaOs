#!/bin/sh
set -eu

source=$1
repo_root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
allowed_tmp=$(realpath -m -- \
    "$repo_root/.tmp/project/paged-memory-and-task-supervision/stage-e/unit3-cut-a")
requested_tmp="${MYOS_PROJECT_TMP:-$allowed_tmp}"
case "/$requested_tmp/" in
    */../*)
        printf '%s\n' '[audit] reject: MYOS_PROJECT_TMP contains ..' >&2
        exit 1
        ;;
esac
case "$requested_tmp" in
    /*) candidate_tmp=$requested_tmp ;;
    *) candidate_tmp=$repo_root/$requested_tmp ;;
esac
project_tmp=$(realpath -m -- "$candidate_tmp")
case "$project_tmp" in
    "$allowed_tmp"|"$allowed_tmp"/*) ;;
    *)
        printf '%s\n' \
            '[audit] reject: MYOS_PROJECT_TMP escapes the task subtree' >&2
        exit 1
        ;;
esac
mkdir -p "$project_tmp"

line() {
    rg -n "$1" "$source" | head -1 | cut -d: -f1 || true
}

continue_start=$(line 'auto continue_close\(TaskId id\) noexcept')
capacity_line=$(line 'constexpr auto capacity\(\) const noexcept -> size_t')
[ -n "$continue_start" ] && [ -n "$capacity_line" ]
continue_block=$(sed -n "${continue_start},$((capacity_line - 1))p" "$source")

relative_line() {
    printf '%s\n' "$continue_block" \
        | rg -n "$1" | head -1 | cut -d: -f1 || true
}

reclaimed_line=$(relative_line 'transition\(TaskState::Reclaimed\)')
publication_line=$(relative_line 'publication_type publication')
take_sender_line=$(relative_line 'closing\.take_sender\(\)')
retire_line=$(relative_line 'const bool retire = slot->generation')
vacant_line=$(relative_line 'slot->payload\.template emplace<VacantSlot>\(\)')
advance_line=$(relative_line '\+\+slot->generation')
publish_line=$(relative_line 'publication\.publish\(\)')

[ -n "$reclaimed_line" ] && [ -n "$publication_line" ] \
    && [ -n "$take_sender_line" ] && [ -n "$retire_line" ] \
    && [ -n "$vacant_line" ] && [ -n "$publish_line" ]
[ "$reclaimed_line" -lt "$publication_line" ]
[ "$publication_line" -le "$take_sender_line" ]
[ "$take_sender_line" -lt "$vacant_line" ]
[ "$vacant_line" -lt "$publish_line" ]
if [ -n "$advance_line" ]; then
    [ "$vacant_line" -lt "$advance_line" ]
    [ "$advance_line" -lt "$publish_line" ]
fi

reserve_start=$(rg -n '^    \[\[nodiscard\]\] auto reserve\(' "$source" \
    | tail -1 | cut -d: -f1 || true)
tag_start=$(line 'auto tag\(TaskId id\) const noexcept')
[ -n "$reserve_start" ] && [ -n "$tag_start" ]
reserve_block=$(sed -n "${reserve_start},$((tag_start - 1))p" "$source")
if printf '%s\n' "$reserve_block" | rg -q 'Record record|ClosingRecord closing'; then
    exit 1
fi
reserve_visibility=$(sed -n "$((${reserve_start} - 12)),${reserve_start}p" \
    "$source" | rg '^[[:space:]]*(private|public):' | tail -1 \
    | sed 's/^[[:space:]]*//')
[ "$reserve_visibility" = 'private:' ]
task_table_start=$(line 'class TaskTable final')
[ -n "$task_table_start" ]
table_prefix=$(sed -n "${task_table_start},${reserve_start}p" "$source")
printf '%s\n' "$table_prefix" | rg -q 'friend class TaskBuilder'

begin_start=$(line 'static auto begin\(')
valid_start=$(rg -n '^    \[\[nodiscard\]\] auto valid\(\) const noexcept -> bool' \
    "$source" | tail -1 | cut -d: -f1 || true)
[ -n "$begin_start" ] && [ -n "$valid_start" ]
begin_block=$(sed -n "${begin_start},$((valid_start - 1))p" "$source")
if printf '%s\n' "$begin_block" | rg -q 'TaskRecord|Record record|ClosingRecord closing'; then
    exit 1
fi

if ! rg -q '~CompletionSet\(\) noexcept' "$source" \
    || ! rg -q 'libk_assert\(cell\.state == CompletionCellState::Vacant' "$source" \
    || ! rg -q 'cell\.state == CompletionCellState::Retired' "$source"; then
    exit 1
fi

cancel_start=$(line 'auto cancel\(CompletionId id\) noexcept')
detach_start=$(line 'auto detach\(CompletionId id\) noexcept')
[ -n "$cancel_start" ] && [ -n "$detach_start" ]
cancel_block=$(sed -n "${cancel_start},$((detach_start - 1))p" "$source")
if ! printf '%s\n' "$cancel_block" \
    | rg -q 'cell_ptr->state == CompletionCellState::Detached' \
    || printf '%s\n' "$cancel_block" \
        | rg -q 'cell_ptr->state == CompletionCellState::Reserved'; then
    exit 1
fi

reservation_start=$(line 'class Reservation final')
table_ctor_start=$(line 'TaskTable\(\) noexcept = default')
[ -n "$reservation_start" ] && [ -n "$table_ctor_start" ]
reservation_block=$(sed -n "${reservation_start},$((table_ctor_start - 1))p" \
    "$source")
printf '%s\n' "$reservation_block" \
    | rg -q 'if \(!table_->cancel_reservation\(id_\)\)'
printf '%s\n' "$reservation_block" \
    | rg -q 'Record::ownership_fault\(MYOS_STATUS_BUSY\)'
if printf '%s\n' "$reservation_block" \
    | rg -q 'static_cast<void>\(table_->cancel_reservation'; then
    exit 1
fi
builder_start=$(line 'class TaskBuilder final')
[ -n "$builder_start" ]
builder_block=$(sed -n "${builder_start},\$p" "$source")
printf '%s\n' "$builder_block" \
    | rg -q 'if \(!reservation_->cancel\(\)\)'
printf '%s\n' "$builder_block" \
    | rg -q 'Table::record_type::ownership_fault\(MYOS_STATUS_BUSY\)'

mutation_dir=$(mktemp -d "$project_tmp/audit-task-transaction.XXXXXX")
trap 'rm -rf "$mutation_dir"' EXIT HUP INT TERM
expect_reject() {
    label=$1
    shift
    mutated="$mutation_dir/$label.hpp"
    sed "$@" "$source" > "$mutated"
    if "$0" "$mutated" >/dev/null 2>&1; then
        printf '%s\n' "[audit] FAIL: mutation accepted: $label" >&2
        exit 1
    fi
    printf '%s\n' "[audit] mutation rejected: $label"
}

expect_reject generation-before-destroy \
    -e '/const bool retire = slot->generation/a\        ++slot->generation;'
expect_reject publish-before-destroy \
    -e '/const bool retire = slot->generation/a\        publication.publish();'
expect_reject local-record-reserve \
    -e '/slot\.payload\.template emplace<ActiveSlot>/a\            Record record{};'
expect_reject missing-completion-lifetime-guard \
    -e '/libk_assert.*CompletionCellState::Vacant/d'
expect_reject sender-only-cancel \
    -e '/cell_ptr->state == CompletionCellState::Detached/s/Detached/Reserved/'
expect_reject public-reserve \
    -e '/auto reserve(/i\    public:'
expect_reject reservation-ignored-failure \
    -e '/if (!table_->cancel_reservation(id_))/a\            static_cast<void>(table_->cancel_reservation(id_));'
expect_reject reservation-destructor-fault \
    -e '/Record::ownership_fault(MYOS_STATUS_BUSY);/d'

printf '%s\n' '[audit] OK: Cut A in-place record and finalization order'
