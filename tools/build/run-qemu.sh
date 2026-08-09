#!/bin/sh
set -eu

qemu=$1
timeout=$2
smp=$3
image=$4
status_contract=$5
shift 5

output=$(mktemp)
markers=$(mktemp)
trap 'rm -f "$output" "$markers"' EXIT
initrd=''
exact_failed=0
forbid=''
count_pattern=''
count_expected=''
while [ "$#" -gt 0 ]; do
  if [ "$1" = '--initrd' ]; then
    initrd=$2
    shift 2
    continue
  fi
  if [ "$1" = '--regex' ]; then
    printf '%s\n' "R:$2" >> "$markers"
    shift 2
    continue
  fi
  if [ "$1" = '--exact-failed-zero' ]; then exact_failed=1; shift; continue; fi
  if [ "$1" = '--forbid' ]; then forbid=$2; shift 2; continue; fi
  if [ "$1" = '--count-regex' ]; then count_pattern=$2; count_expected=$3; shift 3; continue; fi
  printf '%s\n' "$1" >> "$markers"
  shift
done

set +e
if [ -n "$initrd" ]; then
  timeout --foreground "$timeout" "$qemu" -machine virt -smp "$smp" -nographic -bios default -kernel "$image" -initrd "$initrd" >"$output" 2>&1
else
  timeout --foreground "$timeout" "$qemu" -machine virt -smp "$smp" -nographic -bios default -kernel "$image" >"$output" 2>&1
fi
status=$?
set -e
cat "$output"

case "$status_contract" in
  timeout) [ "$status" -eq 124 ] || { printf '%s\n' "[qemu] FAIL: status $status, expected timeout" >&2; exit 1; } ;;
  shutdown) [ "$status" -eq 0 ] || { printf '%s\n' "[qemu] FAIL: status $status, expected shutdown" >&2; exit 1; } ;;
  *) printf '%s\n' "[qemu] FAIL: unknown status contract $status_contract" >&2; exit 1 ;;
esac
[ "$exact_failed" -eq 0 ] || rg -q '^failed=0\r?$' "$output" || { printf '%s\n' '[qemu] FAIL: exact builtin terminal line missing' >&2; exit 1; }
[ -z "$forbid" ] || ! rg -F -q "$forbid" "$output" || { printf '%s\n' "[qemu] FAIL: forbidden marker present: $forbid" >&2; exit 1; }
if [ -n "$count_pattern" ]; then count=$(rg -c "$count_pattern" "$output" || true); [ "$count" -eq "$count_expected" ] || { printf '%s\n' "[qemu] FAIL: expected $count_expected matches, got $count" >&2; exit 1; }; fi

while IFS= read -r marker; do
  case "$marker" in
    R:*) rg -q "${marker#R:}" "$output" || { printf '%s\n' "[qemu] FAIL: regex marker missing: ${marker#R:}" >&2; exit 1; } ;;
    *) rg -F -q "$marker" "$output" || { printf '%s\n' "[qemu] FAIL: marker missing: $marker" >&2; exit 1; } ;;
  esac
done < "$markers"
printf '%s\n' '[qemu] OK: expected status and markers observed'
