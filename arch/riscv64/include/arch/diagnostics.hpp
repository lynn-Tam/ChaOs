#pragma once

#include <arch/trap.hpp>
#include <core/types.hpp>

namespace arch {

struct UnwindSeed final {
    usize pc{};
    usize sp{};
    usize frame_pointer{};
    usize return_address{};
};

using CallSiteSnapshot = UnwindSeed;

[[nodiscard, gnu::always_inline]] inline auto capture_call_site() noexcept
    -> CallSiteSnapshot {
    CallSiteSnapshot snapshot{};
    asm volatile("mv %0, sp" : "=r"(snapshot.sp));
    asm volatile("mv %0, s0" : "=r"(snapshot.frame_pointer));
    asm volatile("mv %0, ra" : "=r"(snapshot.return_address));
    snapshot.pc = snapshot.return_address;
    return snapshot;
}

[[nodiscard]] constexpr auto unwind_seed(
    const TrapSnapshot& trap) noexcept -> UnwindSeed {
    // TrapSnapshot GPR order is ra, sp, gp, tp, t0-t2, s0, ...
    return UnwindSeed{
        .pc = trap.pc,
        .sp = trap.gpr[1],
        .frame_pointer = trap.gpr[7],
        .return_address = trap.gpr[0],
    };
}

} // namespace arch
