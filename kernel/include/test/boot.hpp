#pragma once

#include <boot/boot_info.hpp>

namespace kernel {
struct CpuRuntime;
}

namespace kernel::test {

// Built-in tests are an image-selected module. The core boot continuation
// calls this typed boundary in every image; the off provider is a no-op.
void run(const kernel::boot::BootInfo& boot) noexcept;

// Runtime scenario entry. The off provider is a no-op; test images invoke
// the private scenario driver after the boot CPU has published its runtime.
void runtime(kernel::CpuRuntime& cpu) noexcept;

} // namespace kernel::test
