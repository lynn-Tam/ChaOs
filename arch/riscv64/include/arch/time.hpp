#pragma once

#include <base/types.hpp>
#include <expected>
#include <time/time.hpp>

namespace arch {

enum class TimerError : u8 {
    NotSupported,
    Rejected,
};

[[nodiscard]] auto read_clock() noexcept -> time::Instant;
[[nodiscard]] auto timer_available() noexcept -> bool;
[[nodiscard]] auto program_timer(time::Instant deadline) noexcept
    -> std::expected<void, TimerError>;
void mask_timer() noexcept;

} // namespace arch
