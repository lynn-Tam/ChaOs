#pragma once

#ifdef __ASSEMBLER__
#define MYOS_U64_C(value) value
#else
#include <stdint.h>

typedef uint64_t myos_cap_t;
#define MYOS_U64_C(value) UINT64_C(value)
#endif

#define MYOS_RIGHT_DUPLICATE     (MYOS_U64_C(1) << 0)
#define MYOS_RIGHT_DELEGATE      (MYOS_U64_C(1) << 1)
#define MYOS_RIGHT_RESERVE       (MYOS_U64_C(1) << 2)
#define MYOS_RIGHT_CREATE_REGION (MYOS_U64_C(1) << 3)
#define MYOS_RIGHT_MAP           (MYOS_U64_C(1) << 4)
#define MYOS_RIGHT_UNMAP         (MYOS_U64_C(1) << 5)
#define MYOS_RIGHT_PROTECT       (MYOS_U64_C(1) << 6)
#define MYOS_RIGHT_DESTROY       (MYOS_U64_C(1) << 7)
#define MYOS_RIGHT_INSPECT       (MYOS_U64_C(1) << 8)
#define MYOS_RIGHT_CONTROL       (MYOS_U64_C(1) << 9)
#define MYOS_RIGHT_MANAGE        (MYOS_U64_C(1) << 10)
#define MYOS_RIGHT_REVOKE        (MYOS_U64_C(1) << 11)
