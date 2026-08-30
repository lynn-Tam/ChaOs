#!/bin/sh
set -eu

source=$1
manifest=${2:-}
repo_root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
project_tmp="$repo_root/.tmp/project/paged-memory-and-task-supervision/stage-e/unit3-cut-b"
policy=$(CDPATH= cd -- "$(dirname "$source")" && pwd)/cap_attenuation.hpp
task_source="$repo_root/user/lib/task_transaction.hpp"
space_source="$repo_root/user/lib/deployment.hpp"
mkdir -p "$project_tmp"

audit_one() {
    input=$1
    rg -q 'struct AuthorityId' "$input" || return 1
    rg -q 'class Registration' "$input" || return 1
    rg -q 'class RegistrationJournal' "$input" || return 1
    rg -q 'journal_\.retire_all\(\)' "$input" || return 1
    rg -q 'entry->retiring = true' "$input" || return 1
    rg -q 'entry->leases != 0' "$input" || return 1
    rg -q 'import->mode == MYOS_DEPLOY_IMPORT_MOVE' "$input" || return 1
    rg -q 'authorities\.lease\(' "$input" || return 1
    rg -q 'source\.cspace != 0' "$input" || return 1
    rg -q 'const auto source = space\.lookup\(slot, ceiling\.kind\)' "$input" || return 1
    rg -q 'return owner_\.register_source' "$input" || return 1
    rg -q 'template<typename Authorities, typename Space>' "$input" || return 1
    if rg -q 'journal\(\)' "$input"; then return 1; fi
    rg -q 'B::duplicate' "$input" || return 1
    rg -q 'B::typed_delegate' "$input" || return 1
    rg -q 'B::channel_mint' "$input" || return 1
    rg -q 'adopt_remote_index' "$input" || return 1
    rg -q 'space\.close_remote' "$input" || return 1
    rg -q 'bindings\[index\]\.descriptor' "$input" || return 1
    rg -q 'leases\[index\]->valid\(\)' "$input" || return 1
    rg -q 'attenuation_within_ceiling' "$input" || return 1
    rg -q 'if \(result\.value == 0\)' "$input" || return 1
    rg -q 'Owner owner' "$input" || return 1
    rg -q 'if \(result\.status != MYOS_STATUS_OK\)' "$input" || return 1
    rg -q 'owner\.close\(\)' "$input" || return 1
    rg -q 'Backend::ownership_fault' "$input" || return 1
    rg -q 'adopt\(Space&& source\)' "$input" || return 1
    rg -q 'source\.phase\(\) != Phase::Open' "$input" || return 1
    rg -q 'space_ = libk::move\(source\)' "$input" || return 1
    rg -q 'owner_\.close\(space_\)' "$input" || return 1
    awk '
        /if \(result\.value == 0\)/ { zero = NR }
        /Owner owner/ { owner = NR }
        /if \(result\.status != MYOS_STATUS_OK\)/ { status = NR }
        END { exit !(zero && owner && status && zero < owner && owner < status) }
    ' "$input" || return 1

    awk '
        /class AuthoritySet final/ { in_set = 1 }
        in_set && /^public:/ { access = "public" }
        in_set && /^private:/ { access = "private" }
        in_set && /auto register_source\(/ {
            found = 1
            if (access != "private") bad = 1
        }
        in_set && /auto (source|ceiling|lease_valid)\(AuthorityId/ {
            projections = 1
            if (access != "private") bad = 1
        }
        END { exit (found && projections && !bad) ? 0 : 1 }
    ' "$input" || return 1
}

audit_task_source() {
    input=$1
    # TaskBuilder/TaskRecord no longer expose an arbitrary LocalSlot,
    # identity and ceiling registration seam.  The deployment path owns the sole Task
    # publication registration path; construction must not grow a second
    # registration owner.
    if rg -q 'auto register_source\(' "$input" \
        || rg -q 'reservation_->record\(\)\.register_source' "$input"; then
        return 1
    fi
    if rg -q 'registration_journal' "$input"; then return 1; fi
}

audit_task_space_source() {
    input=$1
    # Local selectors are adopted only by the TaskSpace that owns their
    # aggregate.  Keep this gate on the actual definition rather than on a
    # caller's use-site or a removed Builder wrapper.
    block=$(sed -n '/\[\[nodiscard\]\] auto adopt_local(/,/^    }$/p' "$input")
    [ -n "$block" ] || return 1
    printf '%s\n' "$block" | rg -q 'phase_ != Phase::Open' || return 1
    printf '%s\n' "$block" | rg -q 'owner\.cspace\(\) != 0' || return 1
    printf '%s\n' "$block" \
        | rg -q 'caps_\.adopt_local_slot\(libk::move\(owner\)\)' \
        || return 1
    printf '%s\n' "$block" | rg -q 'if \(!index\)' || return 1
}

audit_policy() {
    input=$1
    rg -q 'enum class DescriptorForm' "$input" || return 1
    rg -q 'DescriptorForm::Ceiling' "$input" || return 1
    rg -q 'DescriptorForm::DuplicateRequest' "$input" || return 1
    rg -q 'DescriptorForm::TypedRequest' "$input" || return 1
    rg -q 'valid_descriptor' "$input" || return 1
    rg -q 'valid_access' "$input" || return 1
    rg -q 'valid_types' "$input" || return 1
    rg -q 'inner_count <= outer_base \+ outer_count - inner_base' "$input" || return 1
    rg -q 'requested\.words\[2\] & ~ceiling\.words\[2\]' "$input" || return 1
    rg -q 'requested\.words\[3\] & ~ceiling\.words\[3\]' "$input" || return 1
    rg -q 'requested\.words\[1\] <= ceiling\.words\[1\]' "$input" || return 1
    rg -q 'requested\.words\[1\] & ceiling\.words\[1\]' "$input" || return 1
    rg -q 'ceiling_unbound' "$input" || return 1
    rg -q 'channel_mint_within' "$input" || return 1
    rg -q 'requested\.words\[0\] <= ceiling\.words\[0\]' "$input" || return 1
}

audit_one "$source"
audit_task_source "$task_source"
audit_task_space_source "$space_source"
if [ -n "$manifest" ]; then
    rg -q 'mode >= MYOS_DEPLOY_IMPORT_MOVE' "$manifest"
fi

mutation_dir=$(mktemp -d "$project_tmp/audit-task-authority.XXXXXX")
trap 'rm -rf "$mutation_dir"' EXIT HUP INT TERM
expect_reject() {
    label=$1
    shift
    mutated="$mutation_dir/$label.hpp"
    sed "$@" "$source" > "$mutated"
    if audit_one "$mutated" >/dev/null 2>&1; then
        printf '%s\n' "[audit] FAIL: mutation accepted: $label" >&2
        exit 1
    fi
    printf '%s\n' "[audit] mutation rejected: $label"
}

expect_reject missing-move-gate \
    -e '/import->mode == MYOS_DEPLOY_IMPORT_MOVE/d' \
    -e '/import->mode >= MYOS_DEPLOY_IMPORT_MOVE/d'
expect_reject missing-lease-gate -e '/auto lease = authorities\.lease/d'
expect_reject missing-source-current \
    -e '/source\.cspace != 0/d'
expect_reject missing-registration-owner-lookup \
    -e '/space\.lookup(slot, ceiling\.kind)/d'
expect_reject public-raw-authority-registration \
    -e '/class AuthoritySet final/,/template<size_t>/{s/^private:$/public:/;}'
expect_reject public-unleased-source \
    -e '/auto source(AuthorityId/ i public:'
expect_reject missing-remote-adoption \
    -e '/space\.adopt_remote_index/,+2d'
expect_reject missing-rollback-close \
    -e '/space\.close_remote/d'
expect_reject missing-descriptor-check \
    -e '/bindings\[index\]\.descriptor/d'
expect_reject missing-result-owner \
    -e '/Owner owner/d'
expect_reject ignored-nonzero-result-branch \
    -e 's/if (result\.value == 0)/if (result.status != MYOS_STATUS_OK || result.value == 0)/'
expect_reject missing-result-close-guard \
    -e '/if (result\.status != MYOS_STATUS_OK)/d'
expect_reject missing-ownership-fault \
    -e '/Backend::ownership_fault/d'
expect_reject missing-bootstrap-adopt \
    -e '/adopt(Space&& source)/d'
expect_reject missing-bootstrap-open-check \
    -e '/source\.phase() != Phase::Open/d'

task_mutation_dir="$mutation_dir/task"
mkdir -p "$task_mutation_dir"
mutated_task="$task_mutation_dir/deployment.hpp"
sed '/caps_\.adopt_local_slot(libk::move(owner))/d' "$space_source" > "$mutated_task"
if audit_task_space_source "$mutated_task" >/dev/null 2>&1; then
    printf '%s\n' '[audit] FAIL: mutation accepted: task adoption owner path' >&2
    exit 1
fi
printf '%s\n' '[audit] mutation rejected: task adoption owner path'

audit_policy "$policy"
policy_mutation_dir="$mutation_dir/policy"
mkdir -p "$policy_mutation_dir"
expect_policy_reject() {
    label=$1
    shift
    mutated="$policy_mutation_dir/$label.hpp"
    sed "$@" "$policy" > "$mutated"
    if audit_policy "$mutated" >/dev/null 2>&1; then
        printf '%s\n' "[audit] FAIL: policy mutation accepted: $label" >&2
        exit 1
    fi
    printf '%s\n' "[audit] policy mutation rejected: $label"
}

expect_policy_reject missing-range-relation \
    -e '/inner_count <= outer_base + outer_count - inner_base/d'
expect_policy_reject missing-memory-access \
    -e '/requested\.words\[2\] & ~ceiling\.words\[2\]/d'
expect_policy_reject missing-memory-types \
    -e '/requested\.words\[3\] & ~ceiling\.words\[3\]/d'
expect_policy_reject missing-pool-budget \
    -e '/requested\.words\[1\] <= ceiling\.words\[1\]/d'
expect_policy_reject missing-endpoint-fixed \
    -e '/requested\.words\[1\] & ceiling\.words\[1\]/d'
expect_policy_reject missing-channel-state \
    -e '/ceiling_unbound/d'
expect_policy_reject missing-channel-mint \
    -e '/channel_mint_within/d'
expect_policy_reject missing-pager-limit \
    -e '/requested\.words\[0\] <= ceiling\.words\[0\]/d'

if [ -n "$manifest" ]; then
    audit_manifest() {
        rg -q 'mode >= MYOS_DEPLOY_IMPORT_MOVE' "$1"
    }
    mutated="$mutation_dir/manifest.hpp"
    sed '/mode >= MYOS_DEPLOY_IMPORT_MOVE/d' "$manifest" > "$mutated"
    if audit_manifest "$mutated"; then
        printf '%s\n' '[audit] FAIL: parser Move mutation accepted' >&2
        exit 1
    fi
    printf '%s\n' '[audit] mutation rejected: parser Move gate'
fi

printf '%s\n' '[audit] OK: source registration, admission and import rollback gates'
