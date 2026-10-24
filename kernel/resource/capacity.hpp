#pragma once

#include <libk/types.hpp>

namespace resource {

// Logical admission capacity. A charge does not reserve future PMM frames.
struct Capacity final {
    u64 memory{};
    u64 caps{};

    [[nodiscard]] constexpr auto empty() const noexcept -> bool {
        return memory == 0 && caps == 0;
    }
    [[nodiscard]] constexpr auto contains(Capacity amount) const noexcept -> bool {
        return memory >= amount.memory && caps >= amount.caps;
    }
    friend constexpr auto operator==(Capacity, Capacity) noexcept -> bool = default;
};

enum class errc : u8 { invalid, closed, exhausted };

} // namespace resource
