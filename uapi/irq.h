#pragma once
#include <stdint.h>

#define IRQ_PLIC 1U
struct IrqConfig {
    uint32_t protocol, sources;
    uint64_t address, bytes, context;
    uint32_t cpu, reserved;
};
