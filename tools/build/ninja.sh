#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "$script_dir/../.." && pwd)
build_dir="$project_root/build/riscv64"
cache_dir="$project_root/build/ccache"

mkdir -p "$cache_dir"
export CCACHE_DIR="$cache_dir"

exec ninja -C "$build_dir" "$@"
