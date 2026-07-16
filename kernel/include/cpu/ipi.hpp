#pragma once

namespace kernel {

struct CpuRuntime;

// Generic software-IPI demultiplexing. Architecture code owns interrupt
// recognition and acknowledgement; each kernel subsystem drains only its
// canonical per-CPU work queue.
void handle_ipi(CpuRuntime& runtime) noexcept;

} // namespace kernel
