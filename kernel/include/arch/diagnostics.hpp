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

} // namespace arch

#include <arch/backend/diagnostics.hpp>

namespace arch {

[[nodiscard, gnu::always_inline]] inline auto capture_call_site() noexcept
    -> CallSiteSnapshot {
    CallSiteSnapshot snapshot{};
    backend::capture_call_site(snapshot);
    return snapshot;
}

[[nodiscard]] constexpr auto unwind_seed(
    const TrapSnapshot& trap) noexcept -> UnwindSeed {
    return backend::trap_unwind_seed<TrapSnapshot, UnwindSeed>(trap);
}

} // namespace arch
