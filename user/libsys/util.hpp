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

class rt_clock final {
    uint64_t frequency_{};
public:
    auto open() noexcept -> status_t {
        const auto result = sys::clock_frequency();
        if (result.status == STATUS_OK) frequency_ = result.value;
        return result.status;
    }
    auto after_ms(uint64_t milliseconds) const noexcept -> std::optional<uint64_t> {
        if (frequency_ == 0) return std::nullopt;
        const auto seconds = libk::checked_multiply(milliseconds / 1000, frequency_);
        const auto fraction = libk::checked_multiply(milliseconds % 1000, frequency_);
        if (!seconds || !fraction) return std::nullopt;
        const auto duration = libk::checked_add(*seconds, *fraction / 1000 + (*fraction % 1000 != 0));
        const auto now = sys::clock_now();
        return duration && now.status == STATUS_OK
            ? libk::checked_add(now.value, *duration) : std::nullopt;
    }
};
