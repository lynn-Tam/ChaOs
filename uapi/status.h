#pragma once

// Register ABI constants shared by C/C++ and preprocessed assembly.
#ifndef __ASSEMBLER__
#include <uapi/types.h>
#endif

#define STATUS_OK               0
#define STATUS_INVALID_CAP     -1
#define STATUS_INVALID_OP      -2
#define STATUS_BAD_RIGHTS      -3
#define STATUS_BAD_ARGS        -4
#define STATUS_NOT_FOUND       -5
#define STATUS_DENIED          -6
#define STATUS_BUSY            -7
#define STATUS_NO_MEMORY       -8
#define STATUS_PENDING         -9
#define STATUS_RETRY          -10
#define STATUS_BACKING_FAILED -11
#define STATUS_INTERNAL       -12
#define STATUS_CLOSED         -13
#define STATUS_REASSERTED     -14
#define STATUS_ALREADY_CONNECTED -15
#define STATUS_WOULD_BLOCK      -16
#define STATUS_PEER_FAULT       -17
#define STATUS_CANCELED         -18
#define STATUS_PEER_ABORTED      -19
#define STATUS_TRANSFER_FAILED   -20
#define STATUS_TIMED_OUT         -21
#define STATUS_PEER_CLOSED       -22
#define STATUS_DIRTY             -23
