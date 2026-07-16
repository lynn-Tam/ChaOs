#pragma once

#include <core/types.hpp>
#include <libk/limits.hpp>
#include <libk/optional.hpp>

namespace kernel::mm {

struct ObjectRange final {
    usize first{};
    usize page_count{};

    [[nodiscard]] constexpr auto end() const noexcept
        -> libk::optional<usize> {
        return page_count != 0
            && first <= libk::numeric_limits<usize>::max() - page_count
            ? libk::optional<usize>{first + page_count}
            : libk::nullopt;
    }
    [[nodiscard]] constexpr auto within(usize pages) const noexcept -> bool {
        const auto limit = end();
        return limit && *limit <= pages;
    }
    [[nodiscard]] constexpr auto contains(ObjectRange other) const noexcept
        -> bool {
        const auto limit = end();
        const auto other_limit = other.end();
        return limit && other_limit
            && other.first >= first && *other_limit <= *limit;
    }
    [[nodiscard]] constexpr auto intersects(ObjectRange other) const noexcept
        -> bool {
        const auto limit = end();
        const auto other_limit = other.end();
        return limit && other_limit
            && first < *other_limit && other.first < *limit;
    }

    [[nodiscard]] friend constexpr auto operator==(
        ObjectRange, ObjectRange) noexcept -> bool = default;
};

} // namespace kernel::mm
