#include <trap.hpp>
#include <algorithm>
#include <cpu.hpp>
#include <csr.hpp>
#include <libk/mem.h>
#include <memory>
#include <pte.hpp>
#include <uapi/riscv64.h>

extern "C" void trap_entry();
extern "C" [[noreturn]] void user_return(arch::TrapFrame*) noexcept;

namespace arch {
void TrapCtx::complete_breakpoint() noexcept {
    // Supervisor-only: the executed parcel is mapped; user breakpoints are faults.
    u16 first{};
    memcpy(&first, reinterpret_cast<const void*>(frame_.sepc), sizeof(first));
    frame_.sepc += instruction_size(first);
}

bool valid_user_start(UserStart s) noexcept {
    return (s.entry >= RISCV64_LOW_GUARD_END && s.entry < PtUserEnd) && (s.entry & 1) == 0
        && s.stack >= RISCV64_LOW_GUARD_END && s.stack <= PtUserEnd
        && (s.stack & 15) == 0;
}

std::optional<usize> prepare_user_stack(usize top, UserStart start) noexcept {
    if (top < sizeof(TrapFrame) || (top & 15) != 0 || !valid_user_start(start)) return {};
    auto* frame = std::construct_at(reinterpret_cast<TrapFrame*>(top - sizeof(TrapFrame)));
    frame->sepc = start.entry;
    frame->gpr[1] = start.stack;
    std::copy(start.arguments.begin(), start.arguments.end(), frame->gpr.begin() + 9);
    // User input never supplies supervisor status bits.
    frame->sstatus = csr::Sstatus::SPIE;
    return reinterpret_cast<usize>(frame);
}

[[noreturn]] void resume_user(usize top) noexcept {
    auto* f = reinterpret_cast<TrapFrame*>(top - sizeof(TrapFrame));
    libk_assert(valid_user_start({.entry = f->sepc, .stack = f->gpr[1]})
                && f->sstatus == csr::Sstatus::SPIE);
    user_return(f);
}

bool install_trap() noexcept {
    csr::Stvec::install_direct(reinterpret_cast<void*>(&trap_entry));
    return csr::Stvec::base() == reinterpret_cast<usize>(&trap_entry);
}
} // namespace arch

auto arch::decode(const arch::TrapFrame& f) noexcept -> arch::TrapEvent {
    using csr::Scause;
    using csr::Sstatus;
    using Cause = TrapEvent::Cause;
    using Perm = TrapEvent::Perm;
    const bool user = !(f.sstatus & Sstatus::SPP);
    auto code = Scause::code(f.scause);
    if (Scause::is_interrupt(f.scause)) {
        auto kind = Cause::UnknownIrq;
        switch (static_cast<Scause::Interrupt>(code)) {
        case Scause::Interrupt::SupervisorTimer: kind = Cause::Timer; break;
        case Scause::Interrupt::SupervisorExternal: kind = Cause::External; break;
        case Scause::Interrupt::SupervisorSoftware: kind = Cause::Ipi; break;
        default: break;
        }
        return {user, f.sepc, 0, kind};
    }
    Cause kind = Cause::Unknown;
    Perm perm = Perm::None;
    switch (static_cast<Scause::Exception>(code)) {
    case Scause::Exception::InstructionAddressMisaligned: kind = Cause::Misaligned; perm = Perm::Execute; break;
    case Scause::Exception::LoadAddressMisaligned: kind = Cause::Misaligned; perm = Perm::Read; break;
    case Scause::Exception::StoreAddressMisaligned: kind = Cause::Misaligned; perm = Perm::Write; break;
    case Scause::Exception::InstructionAccessFault: kind = Cause::AccessFault; perm = Perm::Execute; break;
    case Scause::Exception::LoadAccessFault: kind = Cause::AccessFault; perm = Perm::Read; break;
    case Scause::Exception::StoreAccessFault: kind = Cause::AccessFault; perm = Perm::Write; break;
    case Scause::Exception::InstructionPageFault: kind = Cause::PageFault; perm = Perm::Execute; break;
    case Scause::Exception::LoadPageFault: kind = Cause::PageFault; perm = Perm::Read; break;
    case Scause::Exception::StorePageFault: kind = Cause::PageFault; perm = Perm::Write; break;
    case Scause::Exception::IllegalInstruction: kind = Cause::Illegal; break;
    case Scause::Exception::Breakpoint: kind = Cause::Breakpoint; break;
    case Scause::Exception::UserEnvCall: kind = Cause::Syscall; break;
    default: break;
    }
    return {user, f.sepc, f.stval, kind, perm};
}
