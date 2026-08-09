#!/bin/sh
set -eu

nm=$1
readelf=$2
shift 2

for image in "$@"; do
  if "$nm" -u "$image" | rg -n .; then
    printf '%s\n' "[audit] FAIL: undefined user symbol(s) in $image" >&2
    exit 1
  fi
  if "$nm" --defined-only -n "$image" | rg -n '(_ZTI|_ZTS|_ZTV)'; then
    printf '%s\n' "[audit] FAIL: RTTI/vtable symbol(s) in $image" >&2
    exit 1
  fi
  if "$readelf" -A "$image" | rg -q 'Tag_RISCV_arch:.*(_f|_d|_v)[0-9]'; then
    printf '%s\n' "[audit] FAIL: $image requires F/D/V state" >&2
    "$readelf" -A "$image"
    exit 1
  fi
done
printf '%s\n' '[audit] OK: freestanding integer-only user ELF'
