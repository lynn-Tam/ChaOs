#include <cpu/ipi.hpp>

#include <arch/ipi.hpp>
#include <libk/assert.hpp>
#include <cpu/runtime.hpp>
#include <cpu/registry.hpp>
#include <mm/tlb.hpp>
#include <sched/dispatcher.hpp>

void handle_ipi(CpuRuntime& runtime) noexcept {
    libk_assert(!arch::interrupts_enabled());
    arch::acknowledge_ipi();
    mm::drain_tlb(runtime.local.descriptor->logical_id());
    runtime.dispatcher().drain_remote();
}
