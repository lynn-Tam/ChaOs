// arch/riscv64/trap/trap.cpp

#include "arch/riscv64/cpu/csr.hpp"
#include "arch/riscv64/trap/context.hpp"
#include "arch/riscv64/trap/event.hpp"
#include "arch/riscv64/trap/trapframe.hpp"

#include <arch/trap.hpp>
#include <arch/time.hpp>
#include <arch/cpu.hpp>
#include <panic.hpp>
#include <console.hpp>
#include <trace.hpp>
#include <trap/trap.hpp>
#include <sync.hpp>

using arch::riscv64::TrapFrame;

// trap.S 提供真实入口地址符号。
extern "C" void arch_riscv64_trap_entry();

extern "C" auto arch_riscv64_trap_handler(TrapFrame* frame) noexcept
    -> TrapFrame* {
    libk_assert(frame != nullptr);

    arch::TrapContext context = arch::riscv64::make_context(*frame);
    if (arch::panic_stop_requested()) {
        panic_stop(context);
    }
    const trap::Event event = arch::riscv64::make_event(*frame);
    trace::emit(trace::Event::TrapEnter, static_cast<u64>(event.origin()));
    if (event.origin() != trap::Origin::User || event.interrupt() != nullptr)
        trap::handle(event, context);
    return arch::riscv64::raw_frame(context.frame());
}

extern "C" auto arch_riscv64_trap_exit(TrapFrame* frame) noexcept
    -> TrapFrame* {
    libk_assert(frame != nullptr);
    libk_assert(arch::trap_depth() == 0);

    arch::TrapContext context = arch::riscv64::make_context(*frame);
    sync::assert_unlocked();
    trap::on_exit(arch::riscv64::make_event(*frame), context);
    trace::emit(trace::Event::TrapExit);
    return arch::riscv64::raw_frame(context.frame());
}

extern "C" void arch_riscv64_trap_return(TrapFrame*) noexcept {
    trap::on_return();
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
