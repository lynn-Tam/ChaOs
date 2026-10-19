#pragma once

#include <libk/types.hpp>

namespace resource {

// Capacity values have no dependency on how allocations are owned or closed.
struct budget final {
    u64 memory{};
    u64 caps{};

    [[nodiscard]] constexpr auto empty() const noexcept -> bool {
        return memory == 0 && caps == 0;
    }
    [[nodiscard]] constexpr auto contains(budget amount) const noexcept -> bool {
        return memory >= amount.memory && caps >= amount.caps;
    }
    friend constexpr auto operator==(budget, budget) noexcept -> bool = default;
};

enum class errc : u8 { invalid, closed, exhausted };

} // namespace resource
