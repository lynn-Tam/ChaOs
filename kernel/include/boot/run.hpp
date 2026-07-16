#pragma once
#include <boot/boot_info.hpp>
#include <cpu/topology.hpp>

namespace kernel::boot {

[[noreturn]] void run(BootInfo&& boot, CpuHardwareId boot_cpu) noexcept;

} // namespace kernel::boot
