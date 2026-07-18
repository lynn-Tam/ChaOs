#pragma once

/*
 * BootBundle is a little-endian wire format. These are byte offsets, not a C
 * object layout: readers must decode fields explicitly from bounded storage.
 */
#define MYOS_BOOT_MAGIC UINT64_C(0x544f4f42534f594d) /* "MYOSBOOT" */
#define MYOS_BOOT_MAJOR 1
#define MYOS_BOOT_MINOR 0

#define MYOS_BOOT_ARCH_RISCV64 1
#define MYOS_BOOT_ABI_RISCV_LP64 1

#define MYOS_BOOT_HEADER_SIZE 80
#define MYOS_BOOT_MODULE_SIZE 64
#define MYOS_BOOT_SEGMENT_SIZE 48

#define MYOS_BOOT_MODULE_BOOTABLE (UINT32_C(1) << 0)

#define MYOS_BOOT_SEGMENT_READ    (UINT32_C(1) << 0)
#define MYOS_BOOT_SEGMENT_WRITE   (UINT32_C(1) << 1)
#define MYOS_BOOT_SEGMENT_EXECUTE (UINT32_C(1) << 2)

#ifndef __ASSEMBLER__
#include <stdint.h>

typedef uint64_t myos_boot_features_t;

#endif
