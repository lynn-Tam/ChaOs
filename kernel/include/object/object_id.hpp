#pragma once

#include <core/types.hpp>
#include <uapi/object.h>

namespace kernel::object {

enum class ObjectKind : u16 {
    Invalid = MYOS_OBJECT_KIND_INVALID,
    Thread = MYOS_OBJECT_KIND_THREAD,
    SchedulingContext = MYOS_OBJECT_KIND_SCHED_CONTEXT,
    SchedulingDomain = MYOS_OBJECT_KIND_SCHED_DOMAIN,
    CSpace = MYOS_OBJECT_KIND_CSPACE,
    MemoryObject = MYOS_OBJECT_KIND_MEMORY,
    VSpace = MYOS_OBJECT_KIND_VSPACE,
    ResourcePool = MYOS_OBJECT_KIND_RESOURCE_POOL,
    Notification = MYOS_OBJECT_KIND_NOTIFICATION,
    Vproc = MYOS_OBJECT_KIND_VPROC,
    Tunnel = MYOS_OBJECT_KIND_TUNNEL,
    Endpoint = MYOS_OBJECT_KIND_ENDPOINT,
    Channel = MYOS_OBJECT_KIND_CHANNEL,
    Pager = MYOS_OBJECT_KIND_PAGER,
    Irq = MYOS_OBJECT_KIND_IRQ,
    Count = MYOS_OBJECT_KIND_COUNT,
};

static_assert(static_cast<u16>(ObjectKind::Count)
    == MYOS_OBJECT_KIND_COUNT);

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
