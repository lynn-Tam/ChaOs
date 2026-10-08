#include <cpu/ipi.hpp>

#include <cpu.hpp>
#include <libk/assert.hpp>
#include <cpu/cpu.hpp>
#include <mm/tlb.hpp>
#include <sched/dispatcher.hpp>

void handle_ipi(Cpu& runtime) noexcept {
    libk_assert(!arch::interrupts_enabled());
    arch::acknowledge_ipi();
    mm::drain_tlb(runtime.id);
    runtime.dispatcher().drain_remote();
}

#if !TEST_ENABLED
bool send_ipi(CpuHwId target) noexcept {
    return static_cast<bool>(arch::send_ipi(target));
}
#endif
