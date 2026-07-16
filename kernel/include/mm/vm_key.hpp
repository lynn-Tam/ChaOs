#pragma once

#include <mm/node_pool.hpp>

namespace kernel::mm {

// VSpace-local stable identities. The containing VSpace supplies the identity
// domain; the slot generation prevents ABA when semantic storage is reused.
struct RegionKey final {
    StableNodeKey node{};

    [[nodiscard]] constexpr auto valid() const noexcept -> bool {
        return node.valid();
    }
    [[nodiscard]] friend constexpr auto operator==(
        RegionKey, RegionKey) noexcept -> bool = default;
};

struct MappingKey final {
    StableNodeKey node{};

    [[nodiscard]] constexpr auto valid() const noexcept -> bool {
        return node.valid();
    }
    [[nodiscard]] friend constexpr auto operator==(
        MappingKey, MappingKey) noexcept -> bool = default;
};

} // namespace kernel::mm
