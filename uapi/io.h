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

// Map/unmap are asynchronous. READY acknowledges the last edit; CLOSED
// acknowledges DMA retirement, not device reset or interconnect quiescence.
// Unmap consumes exactly a previously submitted IOVA range.
// Map with IOVA=0 allocates a gap; the result returns its IOVA with PENDING.
