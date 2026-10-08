#pragma once

#include <cpu/types.hpp>

struct Cpu;

// Generic software-IPI demultiplexing. Architecture code owns interrupt
// recognition and acknowledgement; each kernel subsystem drains only its
// canonical per-CPU work queue.
void handle_ipi(Cpu& runtime) noexcept;

// The selected entry supplies the transport; the CPU port implements the ABI.
bool send_ipi(CpuHwId) noexcept;
