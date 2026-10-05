// arch/riscv64/trap/event.cpp
// 这里是 RISC-V scause/sstatus/stval 到 trap::Event 的唯一解码点。

#include "arch/riscv64/trap/event.hpp"

#include "arch/riscv64/cpu/csr.hpp"

namespace {
using arch::riscv64::Scause;
using arch::riscv64::Sstatus;

auto origin_from(usize sstatus) noexcept -> trap::Origin {
    return (sstatus & Sstatus::SPP) == 0 ? trap::Origin::User
                                         : trap::Origin::Kernel;
}

auto exception_from(usize code) noexcept -> trap::Exception {
    switch (static_cast<Scause::Exception>(code)) {
    case Scause::Exception::InstructionAddressMisaligned:
    case Scause::Exception::LoadAddressMisaligned:
    case Scause::Exception::StoreAddressMisaligned:
        return trap::Exception::Misaligned;
    case Scause::Exception::InstructionAccessFault:
    case Scause::Exception::LoadAccessFault:
    case Scause::Exception::StoreAccessFault:
        return trap::Exception::AccessFault;
    case Scause::Exception::IllegalInstruction:
        return trap::Exception::IllegalInstruction;
    case Scause::Exception::Breakpoint:
        return trap::Exception::Breakpoint;
    case Scause::Exception::UserEnvCall:
        return trap::Exception::Syscall;
    case Scause::Exception::InstructionPageFault:
    case Scause::Exception::LoadPageFault:
    case Scause::Exception::StorePageFault:
        return trap::Exception::PageFault;
    default:
        // 未支持的 RISC-V exception 不伪装成已知 kernel policy cause。
        return trap::Exception::Unknown;
    }
}

auto interrupt_from(usize code) noexcept -> trap::Interrupt {
    switch (static_cast<Scause::Interrupt>(code)) {
    case Scause::Interrupt::SupervisorTimer:
        return trap::Interrupt::Timer;
    case Scause::Interrupt::SupervisorExternal:
        return trap::Interrupt::External;
    case Scause::Interrupt::SupervisorSoftware:
        return trap::Interrupt::Software;
    default:
        return trap::Interrupt::Unknown;
    }
}

auto access_from_exception(usize code) noexcept -> trap::Perm {
    switch (static_cast<Scause::Exception>(code)) {
    case Scause::Exception::InstructionAddressMisaligned:
    case Scause::Exception::InstructionAccessFault:
    case Scause::Exception::InstructionPageFault:
        return trap::Perm::Execute;
    case Scause::Exception::LoadAddressMisaligned:
    case Scause::Exception::LoadAccessFault:
    case Scause::Exception::LoadPageFault:
        return trap::Perm::Read;
    case Scause::Exception::StoreAddressMisaligned:
    case Scause::Exception::StoreAccessFault:
    case Scause::Exception::StorePageFault:
        return trap::Perm::Write;
    default:
        return trap::Perm::None;
    }
}
} // namespace

namespace arch::riscv64 {

auto make_event(const TrapFrame& frame) noexcept -> trap::Event {
    const usize raw_cause = frame.scause;
    const usize code = Scause::code(raw_cause);
    const bool interrupt = Scause::is_interrupt(raw_cause);

    const trap::Origin origin = origin_from(frame.sstatus);
    if (interrupt) {
        return trap::Event::interrupt(origin, interrupt_from(code), frame.sepc);
    }

    return trap::Event::exception(
        origin,
        exception_from(code),
        access_from_exception(code),
        frame.sepc,
        frame.stval);
}

} // namespace arch::riscv64
