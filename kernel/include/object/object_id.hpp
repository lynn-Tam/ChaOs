#pragma once

#include <core/types.hpp>

namespace kernel::object {

enum class ObjectKind : u16 {
    Invalid,
    Thread,
    SchedulingContext,
    SchedulingDomain,
    CSpace,
    MemoryObject,
    VSpace,
};

// Kernel-internal stable identity. The address locates a typed store slot; the
// monotonically assigned generation prevents reuse at the same physical page
// from validating a stale identity. A future CSpace selector is not ObjectId
// and must not be treated as authority.
struct ObjectId final {
    usize slot{};
    u64 generation{};
    ObjectKind kind{ObjectKind::Invalid};

    [[nodiscard]] constexpr auto valid() const noexcept -> bool {
        return slot != 0 && generation != 0;
    }

    friend constexpr auto operator==(ObjectId, ObjectId) noexcept
        -> bool = default;
};

} // namespace kernel::object
