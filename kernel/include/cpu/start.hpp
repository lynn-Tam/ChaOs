#pragma once

#include <core/types.hpp>

namespace kernel {

class KernelState;
struct CpuRuntime;

[[noreturn]] void boot_cpu_continue(
    KernelState& kernel,
    CpuRuntime& runtime) noexcept;

[[noreturn]] void cpu_idle_entry(void* argument) noexcept;

extern "C" [[noreturn]] void kernel_secondary_continue(
    CpuRuntime* runtime,
    usize observed_hardware_id) noexcept;

} // namespace kernel
