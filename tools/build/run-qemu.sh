#!/bin/sh
set -eu

qemu=$1
timeout=$2
smp=$3
image=$4
status_contract=$5
shift 5

repo_root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
project_tmp="$repo_root/.tmp/project"
mkdir -p "$project_tmp"
output=$(mktemp "$project_tmp/run-qemu-output.XXXXXX")
markers=$(mktemp "$project_tmp/run-qemu-markers.XXXXXX")
stop_markers=$(mktemp "$project_tmp/run-qemu-stop-markers.XXXXXX")
runner=''
cleanup() {
  if [ -n "$runner" ] && kill -0 "$runner" 2>/dev/null; then
    child=$(ps -o pid= --ppid "$runner" | awk 'NR == 1 { print $1 }')
    [ -z "$child" ] || kill -TERM "$child" 2>/dev/null || true
    kill -TERM "$runner" 2>/dev/null || true
    wait "$runner" 2>/dev/null || true
  fi
  rm -f "$output" "$markers" "$stop_markers"
}
interrupted() {
  cleanup
  exit 130
}
trap cleanup EXIT
trap interrupted HUP INT TERM
initrd=''
memory=''
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
  # /*luna change: carry an explicit QEMU RAM condition, reason: pressure proof must exercise PMM exhaustion without altering ordinary targets*/
  if [ "$1" = '--memory' ]; then
    memory=$2
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
  # An opted-in long-running gate may stop QEMU once a terminal marker is
  # visible, while retaining the outer timeout for silent hangs.
  if [ "$1" = '--stop-when' ]; then printf '%s\n' "$2" >> "$stop_markers"; shift 2; continue; fi
  printf '%s\n' "$1" >> "$markers"
  shift
done

set +e
# /*luna change: apply RAM only on the opted-in run, reason: one wrapper keeps all build and runtime ownership while normal QEMU invocations remain unchanged*/
if [ -n "$initrd" ] && [ -n "$memory" ]; then
  timeout --foreground "$timeout" "$qemu" -machine virt -m "$memory" -smp "$smp" -nographic -bios default -kernel "$image" -initrd "$initrd" >"$output" 2>&1 &
elif [ -n "$initrd" ]; then
  timeout --foreground "$timeout" "$qemu" -machine virt -smp "$smp" -nographic -bios default -kernel "$image" -initrd "$initrd" >"$output" 2>&1 &
elif [ -n "$memory" ]; then
  timeout --foreground "$timeout" "$qemu" -machine virt -m "$memory" -smp "$smp" -nographic -bios default -kernel "$image" >"$output" 2>&1 &
else
  timeout --foreground "$timeout" "$qemu" -machine virt -smp "$smp" -nographic -bios default -kernel "$image" >"$output" 2>&1 &
fi
runner=$!
stopped=0
if [ -s "$stop_markers" ]; then
  while kill -0 "$runner" 2>/dev/null; do
    stop=''
    while IFS= read -r candidate; do
      if rg -F -q "$candidate" "$output"; then
        stop=$candidate
        break
      fi
    done < "$stop_markers"
    if [ -n "$stop" ]; then
      child=$(ps -o pid= --ppid "$runner" | awk 'NR == 1 { print $1 }')
      if [ -n "$child" ] && kill -TERM "$child" 2>/dev/null; then
        stopped=1
        break
      fi
    fi
    sleep 0.1
  done
fi
wait "$runner"
status=$?
set -e
cat "$output"

case "$status_contract" in
  timeout) [ "$status" -eq 124 ] || { printf '%s\n' "[qemu] FAIL: status $status, expected timeout" >&2; exit 1; } ;;
  marker) [ "$stopped" -eq 1 ] || { printf '%s\n' "[qemu] FAIL: terminal marker not observed before status $status" >&2; exit 1; } ;;
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
