#!/bin/sh
set -eu

nm=$1
ar=$2
image=$3
scenario_policy=$4
shift 4

fail() {
  printf '%s\n' "[audit] FAIL: $*" >&2
  exit 1
}

printf '%s\n' '[audit] checking forbidden atomic runtime fallbacks...'
if "$nm" -u "$image" | rg -n '(__atomic_|__sync_)'; then
  fail 'non-lock-free atomic runtime symbol(s) found'
fi
printf '%s\n' '[audit] checking forbidden undefined EH symbols...'
if "$nm" -u "$image" | rg -n '(__gxx_personality_v0|__cxa_throw|__cxa_rethrow|__cxa_begin_catch|_Unwind_)'; then
  fail 'forbidden undefined EH symbol(s) found'
fi
printf '%s\n' '[audit] checking forbidden defined RTTI symbols...'
if "$nm" --defined-only -n "$image" | rg -n '(_ZTI|_ZTS)'; then
  fail 'forbidden defined RTTI symbols found'
fi

printf '%s\n' '[audit] checking panic/assert and console providers...'
"$nm" -C --defined-only -n "$image" \
  | rg -q 'panic\(char const\*' \
  || fail 'kernel panic provider is not linked'
"$nm" -C --defined-only -n "$image" \
  | rg -q 'libk::assert_fail\(' \
  || fail 'common assertion provider is not linked'
"$nm" -C --defined-only -n "$image" \
  | rg -q 'arch::putchar\(' \
  || fail 'kernel console provider is not linked'

"$nm" -C --defined-only -n "$image" | rg -q 'trace::emit\(' || fail 'event recorder is not linked'

case "$scenario_policy" in
  none)
    if "$nm" -C --defined-only "$image" | rg -q 'test::(run\(|runtime\(|scenario::|run_builtin_tests\()'; then
      fail 'scenario driver/state linked into scenario-free image'
    fi
    ;;
  any) ;;
  *) fail "unknown scenario policy: $scenario_policy" ;;
esac

for archive in "$@"; do
  test -f "$archive" || fail "archive is unavailable: $archive"
  case "$archive" in
    *libkernel-core.a|*libkernel-entry.a)
      if "$nm" -C "$archive" | rg -q 'test::'; then
        fail "test dependency in production mechanism archive: $archive"
      fi
      ;;
  esac
done
printf '%s\n' '[audit] OK: symbol and module-boundary checks passed'
