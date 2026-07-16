// arch/riscv64/trap/trap.cpp

#include "arch/riscv64/cpu/csr.hpp"
#include "arch/riscv64/trap/context.hpp"
#include "arch/riscv64/trap/event.hpp"
#include "arch/riscv64/trap/trapframe.hpp"

#include <arch/trap.hpp>
#include <arch/cpu.hpp>
#include <core/debug.hpp>
#include <diag/panic.hpp>
#include <trap/trap.hpp>

using arch::riscv64::TrapFrame;

// trap.S 提供真实入口地址符号。
extern "C" void arch_riscv64_trap_entry();

extern "C" void arch_riscv64_trap_handler(TrapFrame* frame) noexcept {
    KASSERT(frame != nullptr);

    arch::TrapContext context = arch::riscv64::make_context(*frame);
    if (kernel::diag::stop_requested()) {
        kernel::diag::stop_peer(context);
    }
    const kernel::trap::Event event = arch::riscv64::make_event(*frame);
    kernel::trap::handle(event, context);
}

extern "C" void arch_riscv64_trap_exit(TrapFrame* frame) noexcept {
    KASSERT(frame != nullptr);
    KASSERT(arch::trap_depth() == 0);

    arch::TrapContext context = arch::riscv64::make_context(*frame);
    kernel::trap::on_exit(context);
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
