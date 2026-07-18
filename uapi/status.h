#pragma once

// Register ABI constants shared by C/C++ and preprocessed assembly.
#ifndef __ASSEMBLER__
#include <uapi/types.h>
#endif

#define MYOS_STATUS_OK               0
#define MYOS_STATUS_INVALID_CAP     -1
#define MYOS_STATUS_INVALID_OP      -2
#define MYOS_STATUS_BAD_RIGHTS      -3
#define MYOS_STATUS_BAD_ARGS        -4
#define MYOS_STATUS_NOT_FOUND       -5
#define MYOS_STATUS_DENIED          -6
#define MYOS_STATUS_BUSY            -7
#define MYOS_STATUS_NO_MEMORY       -8
#define MYOS_STATUS_PENDING         -9
#define MYOS_STATUS_RETRY          -10
#define MYOS_STATUS_BACKING_FAILED -11
#define MYOS_STATUS_INTERNAL       -12
#define MYOS_STATUS_CLOSED         -13
