#pragma once

#include <libk/checked_arithmetic.hpp>
#include <user/abi/calls.hpp>

namespace myos {
class Clock final {
    uint64_t frequency_{};
public:
    auto open() noexcept -> myos_status_t {
        const auto result = clock_frequency();
        if (result.status == MYOS_STATUS_OK) frequency_ = result.value;
        return result.status;
    }
    auto after_ms(uint64_t milliseconds) const noexcept -> libk::optional<uint64_t> {
        if (frequency_ == 0) return libk::nullopt;
        const auto seconds = libk::checked_multiply(milliseconds / 1000, frequency_);
        const auto fraction = libk::checked_multiply(milliseconds % 1000, frequency_);
        if (!seconds || !fraction) return libk::nullopt;
        const auto duration = libk::checked_add(*seconds, *fraction / 1000 + (*fraction % 1000 != 0));
        const auto now = clock_now();
        return duration && now.status == MYOS_STATUS_OK
            ? libk::checked_add(now.value, *duration) : libk::nullopt;
    }
    // Round up so an absolute tick deadline never precedes the requested time.
    auto after_ns(uint64_t nanoseconds) const noexcept -> libk::optional<uint64_t> {
        if (frequency_ == 0) return libk::nullopt;
        const auto seconds = libk::checked_multiply(nanoseconds / 1'000'000'000, frequency_);
        const auto fraction = libk::checked_multiply(nanoseconds % 1'000'000'000, frequency_);
        if (!seconds || !fraction) return libk::nullopt;
        const auto ticks = libk::checked_add(*seconds,
            *fraction / 1'000'000'000 + (*fraction % 1'000'000'000 != 0));
        const auto now = clock_now();
        return ticks && now.status == MYOS_STATUS_OK
            ? libk::checked_add(now.value, *ticks) : libk::nullopt;
    }
};
} // namespace myos
