#pragma once

#include <core/types.hpp>
#include <uapi/vproc.h>

namespace kernel::operation {

struct Key final {
    u64 raw{};

    [[nodiscard]] constexpr auto valid() const noexcept -> bool {
        return raw != 0;
    }
    [[nodiscard]] constexpr auto slot() const noexcept -> usize {
        return static_cast<usize>(raw & MYOS_OPERATION_SLOT_MASK);
    }
    [[nodiscard]] constexpr auto generation() const noexcept -> u64 {
        return raw >> MYOS_OPERATION_SLOT_BITS;
    }
    [[nodiscard]] friend constexpr auto operator==(
        Key, Key) noexcept -> bool = default;
};

} // namespace kernel::operation
