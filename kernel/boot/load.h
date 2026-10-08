#pragma once
#include <stdint.h>

/* Private bootpack/kernel wire format, little endian, no padding.
 * Opaque payload | Load[] | trailer. Filesystem packages omit the suffix.
 * Load: va, payload offset, file bytes, memory bytes, RWX (five u64).
 * Trailer: magic (RISC-V64, v1), payload bytes, entry, load count (four u64).
 * Source bytes stay inside payload; page-rounded target ranges cannot overlap. */
#define BOOT_LOAD_MAGIC UINT64_C(0x3130544f4f425652)
#define BOOT_LOAD_SIZE 40U
#define BOOT_LOAD_TRAILER 32U
