#!/bin/sh
set -eu

root=$1
revision=$(git -C "$root" rev-parse --short=12 HEAD 2>/dev/null || printf unknown)
if test -n "$(git -C "$root" status --porcelain 2>/dev/null)"; then state=dirty; else state=clean; fi
printf '%s-%s\n' "$revision" "$state"
