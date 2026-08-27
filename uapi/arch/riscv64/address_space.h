#pragma once

/*
 * Public RISC-V 64 target ABI address bounds.  These values are shared by
 * target-specific image validation and the architecture translation layer;
 * they are not fields in any wire format.
 */
#define MYOS_RISCV64_LOW_GUARD_END 0x0000000000010000ULL
#define MYOS_RISCV64_LOWER_CANONICAL_END 0x0000004000000000ULL

