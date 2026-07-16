// RISC-V initial kernel mapping policy.

#pragma once

#include <stdint.h>

#include <libk/expected.hpp>

namespace kernel::mm {
class Pmm;
}

namespace arch::riscv64 {
class Sv39Builder;

enum class InitialKernelMapError : uint8_t {
    InsufficientMemory,
    UnrepresentableAddress,
};

using InitialKernelMapResult = libk::Expected<void, InitialKernelMapError>;

// Maps the kernel substrate needed immediately after the first Sv39
// activation. This is one concrete boot policy, not a general VMM API.
[[nodiscard]] auto map_initial_kernel(Sv39Builder& builder, kernel::mm::Pmm& pmm) noexcept
    -> InitialKernelMapResult;
} // namespace arch::riscv64
