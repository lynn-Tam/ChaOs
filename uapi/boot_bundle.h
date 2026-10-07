#pragma once

/*
 * BootBundle is a little-endian wire format. These are byte offsets, not a C
 * object layout: readers must decode fields explicitly from bounded storage.
 */
#define BUNDLE_MAGIC UINT64_C(0x544f4f42534f594d) /* "MYOSBOOT" */
#define BUNDLE_MAJOR 1
#define BUNDLE_MINOR 1

#define BUNDLE_ARCH_RISCV64 1
#define BUNDLE_ABI_RISCV_LP64 1

#define BUNDLE_HEADER_SIZE 80
#define BUNDLE_MODULE_SIZE 64
#define BUNDLE_SEGMENT_SIZE 48

#define BUNDLE_MODULE_BOOTABLE (UINT32_C(1) << 0)
#define BUNDLE_MODULE_DATA     (UINT32_C(1) << 1)
#define BUNDLE_MODULE_ROLE_MASK \
    (BUNDLE_MODULE_BOOTABLE | BUNDLE_MODULE_DATA)

#define BUNDLE_SEGMENT_READ    (UINT32_C(1) << 0)
#define BUNDLE_SEGMENT_WRITE   (UINT32_C(1) << 1)
#define BUNDLE_SEGMENT_EXECUTE (UINT32_C(1) << 2)

#ifndef __ASSEMBLER__
#include <stdint.h>

typedef uint64_t bundle_flags_t;

#endif
