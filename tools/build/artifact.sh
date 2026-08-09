#!/bin/sh
set -eu

tool=$1
mode=$2
image=$3
output=$4

case "$mode" in
  disasm) "$tool" -d "$image" > "$output" ;;
  symbols) "$tool" -n "$image" > "$output" ;;
  *) printf '%s\n' "unknown artifact mode: $mode" >&2; exit 2 ;;
esac
