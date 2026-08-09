#!/bin/sh
set -eu

build_dir=$1
budget=$2

sources=$(mktemp)
trap 'rm -f "$sources"' EXIT
ninja -C "$build_dir" -t inputs kernel.elf | sed -n 's#^\.\./\.\./\.\./##; /\.cpp$/p' > "$sources"
found=0
failed=0
while IFS= read -r source; do
  stem=$(printf '%s' "${source%.cpp}" | tr / _)
  report=$(find "$build_dir" -name "*${stem}.cpp.su" -type f | head -n 1)
  if [ -z "$report" ]; then
    printf '%s\n' "[audit] FAIL: selected C++ source lacks stack report: $source" >&2
    failed=1
    continue
  fi
  found=1
  while IFS="$(printf '\t')" read -r location frame kind; do
    if [ "$kind" != static ] || [ "$frame" -gt "$budget" ]; then
      printf '%s\n' "[audit] FAIL: $location $frame $kind" >&2
      failed=1
    fi
  done < "$report"
done < "$sources"

[ "$found" -eq 1 ] || { printf '%s\n' '[audit] FAIL: no C++ stack-usage reports found' >&2; exit 1; }
[ "$failed" -eq 0 ] || exit 1
printf '%s\n' "[audit] OK: all C++ frames are static and within $budget bytes"
