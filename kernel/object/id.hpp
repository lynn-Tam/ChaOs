#pragma once

#include <base/types.hpp>
#include <uapi/object.h>

namespace object { class group; }
namespace ipc { class Endpoint; class Channel; class Notification; }
class Pager;
namespace irq { class Irq; }
namespace io { class Device; class Space; }
 class Thread;
namespace sched { class Sc; class Domain; }
namespace cap { class CSpace; }
namespace mm { class Mem; class VSpace; }

namespace object {

enum class ObjectKind : u16 {
    Invalid = MYOS_OBJECT_KIND_INVALID,
    Thread = MYOS_OBJECT_KIND_THREAD,
    Sc = MYOS_OBJECT_KIND_SCHED_CONTEXT,
    Domain = MYOS_OBJECT_KIND_SCHED_DOMAIN,
    CSpace = MYOS_OBJECT_KIND_CSPACE,
    Mem = MYOS_OBJECT_KIND_MEMORY,
    VSpace = MYOS_OBJECT_KIND_VSPACE,
    group = MYOS_OBJECT_KIND_RESOURCE_POOL,
    Notification = MYOS_OBJECT_KIND_NOTIFICATION,
    Endpoint = MYOS_OBJECT_KIND_ENDPOINT,
    Channel = MYOS_OBJECT_KIND_CHANNEL,
    Pager = MYOS_OBJECT_KIND_PAGER,
    Irq = MYOS_OBJECT_KIND_IRQ,
    Device = MYOS_OBJECT_KIND_DEVICE,
    IoSpace = MYOS_OBJECT_KIND_IO_SPACE,
    Count = MYOS_OBJECT_KIND_COUNT,
};

template<typename T>
inline constexpr ObjectKind kind = ObjectKind::Invalid;
template<> inline constexpr ObjectKind kind<object::group> = ObjectKind::group;
template<> inline constexpr ObjectKind kind<ipc::Endpoint> = ObjectKind::Endpoint;
template<> inline constexpr ObjectKind kind<ipc::Channel> = ObjectKind::Channel;
template<> inline constexpr ObjectKind kind<Pager> = ObjectKind::Pager;
template<> inline constexpr ObjectKind kind<irq::Irq> = ObjectKind::Irq;
template<> inline constexpr ObjectKind kind<io::Device> = ObjectKind::Device;
template<> inline constexpr ObjectKind kind<io::Space> = ObjectKind::IoSpace;
template<> inline constexpr ObjectKind kind<ipc::Notification> = ObjectKind::Notification;
template<> inline constexpr ObjectKind kind<Thread> = ObjectKind::Thread;
template<> inline constexpr ObjectKind kind<sched::Sc> = ObjectKind::Sc;
template<> inline constexpr ObjectKind kind<sched::Domain> = ObjectKind::Domain;
template<> inline constexpr ObjectKind kind<cap::CSpace> = ObjectKind::CSpace;
template<> inline constexpr ObjectKind kind<mm::Mem> = ObjectKind::Mem;
template<> inline constexpr ObjectKind kind<mm::VSpace> = ObjectKind::VSpace;

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

} // namespace object
