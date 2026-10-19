#pragma once

#include <uapi/cap.h>
#include <uapi/abi.h>

// System V initial-stack auxv. The SYS keys are this OS's extension.
enum {
    AT_NULL = 0, AT_PAGESZ = 6, AT_ENTRY = 9,
    AT_SYS_POOL = 0x6000, AT_SYS_VSPACE, AT_SYS_CSPACE, AT_SYS_EVENTS,
    AT_SYS_CPUS, AT_SYS_IPC, AT_SYS_BINDINGS, AT_SYS_BINDINGS_SIZE,
    AT_SYS_BOOT, AT_SYS_BOOT_SIZE, AT_SYS_SCOPE, AT_SYS_DOMAIN,
};
struct AuxEntry { word_t type, value; };

#define ROOT_VERSION 2U
#define FW_FDT 1U
// Only the initial root receives this kernel handoff. Ordinary process
// startup and named service bindings are entirely userspace contracts.
struct RootInfo {
    uint32_t version, bytes;
    cap_t domain, thread, sc, bundle, physical;
    uint64_t bundle_bytes;
    cap_t firmware;
    uint64_t firmware_bytes;
    uint32_t firmware_offset, firmware_format;
    uint64_t boot_hart;
};
