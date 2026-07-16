#pragma once

#include <core/types.hpp>
#include <libk/expected.hpp>
#include <time/time.hpp>

namespace arch {

enum class TimerError : u8 {
    NotSupported,
    Rejected,
};

[[nodiscard]] auto read_clock() noexcept -> kernel::time::Instant;
[[nodiscard]] auto timer_available() noexcept -> bool;
[[nodiscard]] auto program_timer(kernel::time::Instant deadline) noexcept
    -> libk::Expected<void, TimerError>;
void mask_timer() noexcept;

} // namespace arch
