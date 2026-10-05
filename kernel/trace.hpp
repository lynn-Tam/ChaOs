#pragma once

#include <base/types.hpp>
#include <array>
#include <libk/sync/atomic.hpp>

struct CpuRuntime;

namespace trace {

enum class Event : u8 {
    State, TrapEnter, TrapExit, Dispatch, Attach, Claim, Complete, Finish,
    Cancel, Post, Take, Kick, KickFail, Ipi,
};

// IDs and arguments come from the actual owner. The recorder never retains
// references, registers an obligation or participates in a state transition.
struct Sample {
    u64 seq{}, tick{}, actor{}, object{}, a{}, b{};
    Event kind{};
};

void emit(Event, u64 actor = 0, u64 object = 0, u64 a = 0, u64 b = 0) noexcept;
[[nodiscard]] auto enabled() noexcept -> bool;
struct Ring {
    struct Cell {
        libk::Atomic<u64> seq{}, tick{}, actor{}, object{}, a{}, b{};
        libk::Atomic<Event> kind{};
    };
    static constexpr usize capacity = 64;
    libk::Atomic<u64> head{};
    std::array<Cell, capacity> cells{};
};

// A fixed sequence window, not a copy of the records. Overwritten/in-progress
// records may fail read(); a live CPU is never stopped or retried by tracing.
struct View {
    const Ring* log{};
    u64 first{}, last{};
    [[nodiscard]] auto read(u64 seq, Sample&) const noexcept -> bool;
};
[[nodiscard]] auto snapshot(const CpuRuntime&) noexcept -> View;

} // namespace trace
