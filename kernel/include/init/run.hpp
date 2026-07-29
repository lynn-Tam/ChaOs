#pragma once
#include <boot/boot_info.hpp>
#include <libk/manual_lifetime.hpp>

namespace kernel {
class CpuRegistry;
}

namespace kernel::init {

[[noreturn]] void run(
    libk::ManualLifetime<kernel::boot::BootInfo>& source) noexcept;

#if MYOS_CONCURRENCY_PROBE == 3 || MYOS_CONCURRENCY_PROBE == 4
//Confirmatory experiment.
// Exit condition: remove with the Stage B operation fault probes.
[[nodiscard]] auto run_concurrency_probe(
    kernel::CpuRegistry& cpus,
    u32 probe) noexcept -> bool;
#endif

} // namespace kernel::init
