#include <cpu/ipi.hpp>

#include <arch/ipi.hpp>
#include <core/debug.hpp>
#include <cpu/cpu_runtime.hpp>
#include <mm/translation.hpp>
#include <sched/dispatcher.hpp>

namespace kernel {

void handle_ipi(CpuRuntime& runtime) noexcept {
    KASSERT(!arch::interrupts_enabled());
    arch::acknowledge_ipi();
    kernel::mm::drain_shootdowns(runtime);
    runtime.dispatcher().drain_remote();
}

} // namespace kernel
