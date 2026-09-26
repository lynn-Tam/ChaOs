#pragma once

#include <stddef.h>
#include <uapi/types.h>

enum myos_io_space_state {
    MYOS_IO_SPACE_EMPTY = 0,
    MYOS_IO_SPACE_BINDING = 1,
    MYOS_IO_SPACE_OPENING = 2,
    MYOS_IO_SPACE_ACTIVE = 3,
    MYOS_IO_SPACE_CLOSING = 4,
    MYOS_IO_SPACE_CLOSED = 5,
    MYOS_IO_SPACE_FAILED = 6,
    MYOS_IO_SPACE_FAULTED = 7,
};

#define MYOS_IO_INFO_VERSION 1
#define MYOS_DEVICE_INFO_VERSION 1

// Inspectable before binding. Requester identifies the platform function;
// configuration never includes writable BAR addresses.
struct myos_device_info {
    uint32_t version;
    uint32_t requester;
    uint32_t configuration[64];
    uint64_t bar_sizes[6];
};

// Discovery snapshot, not live PCI configuration space. BAR address bits are
// absent; a driver obtains register access through IO_SPACE_BAR capabilities.
struct myos_io_info {
    uint32_t version;
    uint32_t reserved;
    uint32_t configuration[64];
    uint64_t bar_sizes[6];
};

#ifdef __cplusplus
static_assert(sizeof(myos_device_info) == 312);
static_assert(sizeof(myos_io_info) == 312);
static_assert(offsetof(myos_device_info, configuration) == 8);
static_assert(offsetof(myos_device_info, bar_sizes) == 264);
#endif
