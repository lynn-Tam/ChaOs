#pragma once

#include <core/types.hpp>
#include <libk/optional.hpp>

namespace kernel::sched {

class Urgency final {
public:
    static constexpr u8 level_count = 32;

    [[nodiscard]] static constexpr auto make(u8 value) noexcept
        -> libk::optional<Urgency> {
        if (value >= level_count) {
            return libk::nullopt;
        }
        return Urgency{value};
    }

    [[nodiscard]] constexpr auto value() const noexcept -> u8 {
        return value_;
    }

    friend constexpr auto operator==(Urgency, Urgency) noexcept
        -> bool = default;

private:
    explicit constexpr Urgency(u8 value) noexcept : value_(value) {}

    u8 value_{};
};

enum class DispatchReason : u8 {
    Start,
    Yield,
    Timer,
    Block,
    Exit,
    Stop,
    RemoteWake,
    Activation,
};

} // namespace kernel::sched
