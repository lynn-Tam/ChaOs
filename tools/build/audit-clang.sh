#!/bin/sh
set -eu

clangxx=$1
root=$2

cd "$root"
find arch/riscv64 kernel libk -name '*.cpp' -type f -print | sort | while IFS= read -r source; do
  "$clangxx" --target=riscv64-unknown-elf \
    -ffreestanding -Wall -Wextra -O2 -g3 -march=rv64gc -mabi=lp64d \
    -mcmodel=medany -msmall-data-limit=0 -I . -I kernel -I kernel/include \
    -I arch/riscv64/include -std=gnu++2b -fno-exceptions -fno-rtti \
    -fno-threadsafe-statics -fno-use-cxa-atexit -fsyntax-only "$source"
done
printf '%s\n' '[audit] OK: Clang RISC-V freestanding syntax passed'
