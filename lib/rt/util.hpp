#pragma once

#include <start.hpp>
#include <sys/syscall.hpp>
#include <libk/checked_arithmetic.hpp>

// Callers choose the duration; no runtime state or implicit timeout policy.
inline auto rt_after_ms(uint64_t milliseconds) noexcept -> std::optional<uint64_t> {
    const auto frequency = sys::clock_frequency();
    if (frequency.status != STATUS_OK || !frequency.value)
        return std::nullopt;
    const auto seconds = libk::checked_multiply(milliseconds / 1000, frequency.value);
    const auto fraction = libk::checked_multiply(milliseconds % 1000, frequency.value);
    if (!seconds || !fraction)
        return std::nullopt;
    const auto duration = libk::checked_add(*seconds, *fraction / 1000 + (*fraction % 1000 != 0));
    const auto now = sys::clock_now();
    return duration && now.status == STATUS_OK ? libk::checked_add(now.value, *duration)
                                               : std::nullopt;
}

// One monotonic admission limit. Zero permits an unbounded request; after
// admission, completion and resource retirement remain separate obligations.
inline auto rt_admit(uint64_t deadline) noexcept -> status_t {
    if (!deadline)
        return STATUS_OK;
    const auto now = sys::clock_now();
    return now.status != STATUS_OK ? now.status
           : now.value >= deadline ? STATUS_TIMED_OUT
                                   : STATUS_OK;
}
