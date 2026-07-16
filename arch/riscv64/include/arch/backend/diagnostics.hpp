#pragma once

#include <core/types.hpp>

namespace arch::backend {

template<typename Snapshot>
    requires requires(Snapshot& value) {
        value.pc;
        value.sp;
        value.frame_pointer;
        value.return_address;
    }
[[gnu::always_inline]] inline void capture_call_site(
    Snapshot& snapshot) noexcept {
    asm volatile("mv %0, sp" : "=r"(snapshot.sp));
    asm volatile("mv %0, s0" : "=r"(snapshot.frame_pointer));
    asm volatile("mv %0, ra" : "=r"(snapshot.return_address));
    snapshot.pc = snapshot.return_address;
}

template<typename TrapSnapshot, typename Seed>
    requires requires(const TrapSnapshot& trap, Seed& seed) {
        trap.gpr[0];
        trap.pc;
        seed.pc;
        seed.sp;
        seed.frame_pointer;
        seed.return_address;
    }
[[nodiscard]] constexpr auto trap_unwind_seed(
    const TrapSnapshot& trap) noexcept -> Seed {
    // TrapSnapshot GPR order is ra, sp, gp, tp, t0-t2, s0, ...
    return Seed{
        .pc = trap.pc,
        .sp = trap.gpr[1],
        .frame_pointer = trap.gpr[7],
        .return_address = trap.gpr[0],
    };
}

} // namespace arch::backend
