#include <utility>
#include <cpu/runtime.hpp>

#include <panic.hpp>
#include <trace.hpp>
#include <memory>
#include <libk/assert.hpp>

CpuRuntime::~CpuRuntime() noexcept {
    dispatcher_storage.reset();
    if (idle_thread) {
        libk_assert(idle_thread.retire());
        idle_thread.reset();
    }
    if (log) std::destroy_at(log);
    if (panic) std::destroy_at(panic);
    emergency_stack.reset();
    irq_stack.reset();
    init_stack.reset();
}


auto CpuRuntime::init_log(mm::Pmm& pmm, CpuId id, CpuHwId hw, CpuRegistry& registry) noexcept -> bool {
    static_assert(sizeof(PanicSlot) <= mm::page_size);
    static_assert(sizeof(trace::Ring) <= mm::page_size);
    auto page = pmm.allocate_page();
    if (!page) return false;
    panic_page = std::move(page).value();
    panic = std::construct_at(reinterpret_cast<PanicSlot*>(panic_page.bytes()));
    panic->cpu = id;
    panic->hardware = hw;
    panic->registry = &registry;
    if (trace::enabled()) {
        auto history = pmm.allocate_page();
        if (!history) return false;
        trace_page = std::move(history).value();
        log = std::construct_at(reinterpret_cast<trace::Ring*>(trace_page.bytes()));
    }
    return true;
}
