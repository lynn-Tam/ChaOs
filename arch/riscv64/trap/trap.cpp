// arch/riscv64/trap/trap.cpp

#include "arch/riscv64/cpu/csr.hpp"
#include "arch/riscv64/trap/context.hpp"
#include "arch/riscv64/trap/event.hpp"
#include "arch/riscv64/trap/trapframe.hpp"

#include <arch/trap.hpp>
#include <arch/time.hpp>
#include <arch/cpu.hpp>
#include <core/debug.hpp>
#include <diag/console.hpp>
#include <diag/panic.hpp>
#include <diag/concurrency.hpp>
#include <trap/trap.hpp>
#include <sync/trace.hpp>

using arch::riscv64::TrapFrame;

// trap.S 提供真实入口地址符号。
extern "C" void arch_riscv64_trap_entry();

extern "C" auto arch_riscv64_trap_handler(TrapFrame* frame) noexcept
    -> TrapFrame* {
    KASSERT(frame != nullptr);

    arch::TrapContext context = arch::riscv64::make_context(*frame);
    if (kernel::diag::stop_requested()) {
        kernel::diag::stop_peer(context);
    }
    const kernel::trap::Event event = arch::riscv64::make_event(*frame);
    const u64 entry_tick = (kernel::sync::enabled(
        kernel::sync::Level::Profile)
        || kernel::diag::concurrency::enabled(
            kernel::diag::concurrency::Level::Snapshot))
        ? arch::trap_entry_tick() : 0;
    if (kernel::sync::enabled(kernel::sync::Level::Verify)) {
        kernel::sync::trap_enter(
            event,
            kernel::sync::enabled(kernel::sync::Level::Profile)
                ? entry_tick : 0);
    }
    if (kernel::diag::concurrency::enabled(
            kernel::diag::concurrency::Level::Snapshot)) {
        kernel::diag::concurrency::trap_enter(
            entry_tick, static_cast<u32>(event.origin()));
    }
    kernel::trap::handle(event, context);
    return arch::riscv64::raw_frame(context.frame());
}

extern "C" auto arch_riscv64_trap_exit(TrapFrame* frame) noexcept
    -> TrapFrame* {
    KASSERT(frame != nullptr);
    KASSERT(arch::trap_depth() == 0);

    arch::TrapContext context = arch::riscv64::make_context(*frame);
    if (kernel::sync::enabled(kernel::sync::Level::Verify)) {
        kernel::sync::trap_exiting();
        kernel::sync::assert_no_locks();
        // The trap's CPU-local diagnostic frame must be retired before
        // on_exit may dispatch another execution.  A scheduler switch can
        // return through this hook much later; keeping the old frame live
        // would make unrelated user traps look recursively nested.
        kernel::sync::trap_exit(
            kernel::sync::enabled(kernel::sync::Level::Profile)
                ? arch::read_clock().ticks() : 0);
    }
    if (kernel::diag::concurrency::enabled(
            kernel::diag::concurrency::Level::Snapshot)) {
        kernel::diag::concurrency::trap_exit(arch::read_clock().ticks());
    }
    kernel::trap::on_exit(context);
    return arch::riscv64::raw_frame(context.frame());
}


namespace arch {

// 声明点：arch/riscv64/include/arch/trap.hpp，把 stvec 指向 trap entry。
[[nodiscard]] auto install_trap() noexcept -> bool {
    riscv64::Stvec::install_direct(
        reinterpret_cast<void*>(&arch_riscv64_trap_entry));
    return riscv64::Stvec::base()
        == reinterpret_cast<usize>(&arch_riscv64_trap_entry);
}

} // namespace arch
