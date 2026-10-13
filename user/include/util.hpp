#pragma once

#include <start.hpp>
#include <sys/syscall.hpp>
#include <libk/checked_arithmetic.hpp>

inline void cap_encode(
    const CapView& value,
    std::span<uint8_t, CAP_ATTENUATION_SIZE> output) noexcept {
    const auto put = [&output](
        size_t offset, uint64_t field, size_t width) noexcept {
        for (size_t byte = 0; byte < width; ++byte) {
            output[offset + byte] = static_cast<uint8_t>(field >> (byte * 8));
        }
    };
    put(CAP_ATTENUATION_VERSION_OFFSET, value.version, 2);
    put(CAP_ATTENUATION_KIND_OFFSET, value.kind, 2);
    put(CAP_ATTENUATION_SIZE_OFFSET, value.size, 4);
    put(CAP_ATTENUATION_RIGHTS_OFFSET, value.rights, 8);
    for (size_t index = 0; index < 6; ++index) {
        put(CAP_ATTENUATION_WORD0_OFFSET + index * 8,
            value.words[index], 8);
    }
}

// Callers choose the duration; no runtime state or implicit timeout policy.
inline auto rt_after_ms(uint64_t milliseconds) noexcept -> std::optional<uint64_t> {
    const auto frequency = sys::clock_frequency();
    if (frequency.status != STATUS_OK || !frequency.value) return std::nullopt;
    const auto seconds = libk::checked_multiply(milliseconds / 1000, frequency.value);
    const auto fraction = libk::checked_multiply(milliseconds % 1000, frequency.value);
    if (!seconds || !fraction) return std::nullopt;
    const auto duration = libk::checked_add(*seconds, *fraction / 1000 + (*fraction % 1000 != 0));
    const auto now = sys::clock_now();
    return duration && now.status == STATUS_OK
        ? libk::checked_add(now.value, *duration) : std::nullopt;
}
