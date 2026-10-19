#pragma once

#include <uapi/abi.h>
#include <uapi/cap.h>

#define IOMMU_RISCV 1U
enum IoState { IO_SPACE_PENDING, IO_SPACE_READY, IO_SPACE_CLOSING, IO_SPACE_CLOSED, IO_SPACE_FAILED };

struct IommuConfig {
    uint32_t protocol, reserved;
    uint64_t address, bytes;
    cap_t irq_controller;
    uint32_t source, device_bits;
};

// Each accepted edit returns an owned Op. Map with address=0 allocates a gap;
// its Done result returns the IOVA. Unmap consumes exactly a prior map range.
// Close is a prepaid coalesced intent; CLOSED proves DMA retirement, not reset.
#define IO_OP_MAP 1U
#define IO_OP_UNMAP 2U
struct IoReq { cap_t memory; uint64_t page, count, address, perms; };
