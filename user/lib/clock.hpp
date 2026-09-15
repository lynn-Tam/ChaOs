#pragma once

#include <libk/checked_arithmetic.hpp>
#include <user/lib/syscall.hpp>

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
};
inline auto decimal(const char* text) noexcept -> libk::optional<uint64_t> {
    if (text == nullptr || *text == 0) return libk::nullopt;
    uint64_t value{};
    while (*text != 0) {
        if (*text < '0' || *text > '9' || value > (UINT64_MAX - (*text - '0')) / 10)
            return libk::nullopt;
        value = value * 10 + (*text++ - '0');
    }
    return value;
}
} // namespace myos
