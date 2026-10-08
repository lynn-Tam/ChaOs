#pragma once

#include <stddef.h>
#include <uapi/abi.h>

enum IoState {
    IO_SPACE_EMPTY = 0,
    IO_SPACE_BINDING = 1,
    IO_SPACE_OPENING = 2,
    IO_SPACE_ACTIVE = 3,
    IO_SPACE_CLOSING = 4,
    IO_SPACE_CLOSED = 5,
    IO_SPACE_FAILED = 6,
    IO_SPACE_FAULTED = 7,
};

// PCI BAR indices 0..5; index 6 exports the function's read-only ECAM page.
#define IO_PCI_CFG 6U
#define IO_REG_COUNT 7U
