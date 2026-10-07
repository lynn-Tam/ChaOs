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

#define IO_INFO_VERSION 1
#define DEVICE_INFO_VERSION 1

// Inspectable before binding. Requester identifies the platform function;
// configuration never includes writable BAR addresses.
struct DeviceDesc {
    uint32_t version;
    uint32_t requester;
    uint32_t configuration[64];
    uint64_t bar_sizes[6];
};

// Discovery snapshot, not live PCI configuration space. BAR address bits are
// absent; a driver obtains register access through IO_SPACE_BAR capabilities.
struct IoInfo {
    uint32_t version;
    uint32_t reserved;
    uint32_t configuration[64];
    uint64_t bar_sizes[6];
};

#ifdef __cplusplus
static_assert(sizeof(DeviceDesc) == 312);
static_assert(sizeof(IoInfo) == 312);
static_assert(offsetof(DeviceDesc, configuration) == 8);
static_assert(offsetof(DeviceDesc, bar_sizes) == 264);
#endif
