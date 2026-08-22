#!/bin/sh
set -eu

nm=$1
ar=$2
image=$3
concurrency_level=$4
scenario_policy=$5
shift 5

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
  | rg -q 'kernel::diag::panic\(kernel::diag::PanicRequest\)' \
  || fail 'kernel panic provider is not linked'
"$nm" -C --defined-only -n "$image" \
  | rg -q 'kernel::diag::assert_fail\(' \
  || fail 'KASSERT terminal provider is not linked'
"$nm" -C --defined-only -n "$image" \
  | rg -q 'kernel::diag::console::write\(' \
  || fail 'kernel console provider is not linked'

if [ "$concurrency_level" -eq 0 ]; then
  printf '%s\n' '[audit] checking bounded off concurrency provider...'
  "$nm" -C --defined-only -n "$image" | rg -q 'kernel::diag::concurrency::FlightRecorder::(initialize|push|read)' || fail 'off recorder ABI stubs are not linked'
  if "$nm" -u -C "$image" | rg -q 'kernel::diag::concurrency::FlightRecorder::'; then
    fail 'off recorder ABI remains unresolved'
  fi
fi

case "$scenario_policy" in
  none)
    if "$nm" -C --defined-only "$image" | rg -q 'kernel::test::scenario::detail::'; then
      fail 'scenario driver/state linked into scenario-free image'
    fi
    ;;
  pressure)
    "$nm" -C --defined-only "$image" \
      | rg -q 'kernel::test::scenario::detail::pressure\(' \
      || fail 'pressure scenario provider is not linked'
    "$nm" -C --defined-only "$image" \
      | rg -q 'kernel::test::scenario::detail::page_fault\(' \
      || fail 'pressure PageFault fixture is not linked'
    ;;
  any) ;;
  *) fail "unknown scenario policy: $scenario_policy" ;;
esac

# The core archive is the only target allowed to contain object vtables; the
# module graph keeps diagnostics and test providers free of polymorphic state.
core_archive=$1
shift
"$ar" t "$core_archive" | while IFS= read -r object; do
  if "$nm" --defined-only -n "$object" | rg -q '_ZTV' && ! basename "$object" | rg -q '^kernel_object_'; then
    fail "vtable symbol found outside kernel/object: $(basename "$object")"
  fi
done
for archive in "$@"; do
  if "$nm" --defined-only -n "$archive" | rg -q '_ZTV'; then
    fail "vtable symbol found outside core target: $archive"
  fi
done
test -f "$core_archive" || fail "core archive is unavailable: $core_archive"
printf '%s\n' '[audit] OK: symbol and module-boundary checks passed'
