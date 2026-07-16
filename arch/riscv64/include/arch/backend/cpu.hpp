#pragma once

#include "arch/riscv64/cpu/local_entry.hpp"
#include "arch/riscv64/cpu/start_context.hpp"

namespace arch::backend {

using CpuEntryState = riscv64::CpuEntryBlock;
using CpuStartContext = riscv64::CpuStartContext;

} // namespace arch::backend
