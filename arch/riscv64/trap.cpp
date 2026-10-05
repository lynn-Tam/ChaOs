#include <trap.hpp>
#include <algorithm>
#include <arch/cpu.hpp>
#include <arch/instruction.hpp>
#include <arch/riscv64/cpu/csr.hpp>
#include <libk/mem.h>
#include <memory>
#include <mm/table.hpp>
#include <panic.hpp>
#include <sync.hpp>
#include <trace.hpp>
#include <trap/trap.hpp>

extern "C" void trap_entry();
extern "C" [[noreturn]] void user_return(arch::TrapFrame*) noexcept;

namespace arch {
void TrapCtx::complete_breakpoint() noexcept {
    // Supervisor-only: the executed parcel is mapped; user breakpoints are faults.
    u16 first{};
    memcpy(&first, reinterpret_cast<const void*>(frame_->sepc), sizeof(first));
    frame_->sepc += instruction_size(first);
}

bool valid_user_start(UserStart s) noexcept {
    return mm::is_user(s.entry) && (s.entry.raw() & 1) == 0
        && s.stack.raw() >= mm::UserBegin && s.stack.raw() <= mm::UserEnd
        && (s.stack.raw() & 15) == 0;
}

bool TrapCtx::load_user_start(const UserStart& s) noexcept {
    if (!valid_user_start(s)) return false;
    *frame_ = {};
    frame_->sepc = s.entry.raw();
    frame_->gpr[1] = s.stack.raw();
    std::copy(s.arguments.begin(), s.arguments.end(), frame_->gpr.begin() + 9);
    // User input never supplies supervisor status bits.
    frame_->sstatus = riscv64::Sstatus::SPIE;
    return true;
}

std::optional<TrapFrame*> prepare_user_frame(usize top, UserStart start) noexcept {
    if (top < sizeof(TrapFrame) || (top & 15) != 0 || !valid_user_start(start)) return {};
    auto* f = std::construct_at(reinterpret_cast<TrapFrame*>(top - sizeof(TrapFrame)));
    [[maybe_unused]] bool loaded = TrapCtx{*f}.load_user_start(start);
    libk_assert(loaded);
    return f;
}

std::optional<usize> prepare_user_stack(usize top, UserStart start) noexcept {
    auto f = prepare_user_frame(top, start);
    return f ? std::optional{reinterpret_cast<usize>(*f)} : std::nullopt;
}

[[noreturn]] void resume_user(usize top) noexcept {
    auto* f = reinterpret_cast<TrapFrame*>(top - sizeof(TrapFrame));
    libk_assert(mm::is_user(mm::Virt{f->sepc}) && f->sstatus == riscv64::Sstatus::SPIE
                && (f->gpr[1] & 15) == 0);
    user_return(f);
}

bool install_trap() noexcept {
    riscv64::Stvec::install_direct(reinterpret_cast<void*>(&trap_entry));
    return riscv64::Stvec::base() == reinterpret_cast<usize>(&trap_entry);
}
} // namespace arch

static auto decode(const arch::TrapFrame& f) noexcept -> trap::Event {
    using arch::riscv64::Scause;
    using arch::riscv64::Sstatus;
    using trap::Exception;
    using trap::Perm;
    auto origin = f.sstatus & Sstatus::SPP ? trap::Origin::Kernel : trap::Origin::User;
    auto code = Scause::code(f.scause);
    if (Scause::is_interrupt(f.scause)) {
        auto kind = trap::Interrupt::Unknown;
        switch (static_cast<Scause::Interrupt>(code)) {
        case Scause::Interrupt::SupervisorTimer: kind = trap::Interrupt::Timer; break;
        case Scause::Interrupt::SupervisorExternal: kind = trap::Interrupt::External; break;
        case Scause::Interrupt::SupervisorSoftware: kind = trap::Interrupt::Software; break;
        default: break;
        }
        return trap::Event::interrupt(origin, kind, f.sepc);
    }
    Exception kind = Exception::Unknown;
    Perm perm = Perm::None;
    switch (static_cast<Scause::Exception>(code)) {
    case Scause::Exception::InstructionAddressMisaligned: kind = Exception::Misaligned; perm = Perm::Execute; break;
    case Scause::Exception::LoadAddressMisaligned: kind = Exception::Misaligned; perm = Perm::Read; break;
    case Scause::Exception::StoreAddressMisaligned: kind = Exception::Misaligned; perm = Perm::Write; break;
    case Scause::Exception::InstructionAccessFault: kind = Exception::AccessFault; perm = Perm::Execute; break;
    case Scause::Exception::LoadAccessFault: kind = Exception::AccessFault; perm = Perm::Read; break;
    case Scause::Exception::StoreAccessFault: kind = Exception::AccessFault; perm = Perm::Write; break;
    case Scause::Exception::InstructionPageFault: kind = Exception::PageFault; perm = Perm::Execute; break;
    case Scause::Exception::LoadPageFault: kind = Exception::PageFault; perm = Perm::Read; break;
    case Scause::Exception::StorePageFault: kind = Exception::PageFault; perm = Perm::Write; break;
    case Scause::Exception::IllegalInstruction: kind = Exception::IllegalInstruction; break;
    case Scause::Exception::Breakpoint: kind = Exception::Breakpoint; break;
    case Scause::Exception::UserEnvCall: kind = Exception::Syscall; break;
    default: break;
    }
    return trap::Event::exception(origin, kind, perm, f.sepc, f.stval);
}

extern "C" auto trap_enter(arch::TrapFrame* f) noexcept -> arch::TrapFrame* {
    libk_assert(f);
    arch::TrapCtx ctx{*f};
    if (arch::panic_stop_requested()) panic_stop(ctx);
    auto event = decode(*f);
    trace::emit(trace::Event::TrapEnter, static_cast<u64>(event.origin()));
    if (event.origin() != trap::Origin::User || event.interrupt()) trap::handle(event, ctx);
    return ctx.frame();
}
extern "C" auto trap_exit(arch::TrapFrame* f) noexcept -> arch::TrapFrame* {
    libk_assert(f && arch::trap_depth() == 0);
    arch::TrapCtx ctx{*f};
    sync::assert_unlocked();
    trap::on_exit(decode(*f), ctx);
    trace::emit(trace::Event::TrapExit);
    return ctx.frame();
}
extern "C" void trap_return(arch::TrapFrame*) noexcept { trap::on_return(); }
