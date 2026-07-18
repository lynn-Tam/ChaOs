#pragma once

#include <core/types.hpp>
#include <object/object_id.hpp>
#include <resource/sponsorship.hpp>

namespace kernel::object {

enum class ObjectLifecycle : u8 {
    Free,
    Constructing,
    Live,
    Retiring,
    Quiescent,
    Quarantined,
};

enum class ObjectError : u8 {
    OutOfMemory,
    GenerationExhausted,
    InvalidIdentity,
    InvalidLifecycle,
    WrongKind,
};

class ObjectRef;
class ObjectCleanup;
template<typename T>
class ObjectPin;
template<typename T>
class ObjectPool;

} // namespace kernel::object

namespace kernel::resource {
class Reservation;
class Permit;
class Refund;
class Charge;
class Sponsorship;
class ResourcePool;
}

namespace kernel::object {

// Common storage header for every typed ObjectPool slot. It is deliberately
// outside the payload: kernel objects do not inherit a lifecycle base and do
// not carry capability state. The ops table returns to the actual pool so all
// reference and pin transitions remain serialized by that pool's one lock.
class ObjectAnchor final {
public:
    [[nodiscard]] constexpr auto kind() const noexcept -> ObjectKind {
        return kind_;
    }

    [[nodiscard]] constexpr auto generation() const noexcept -> u64 {
        return generation_;
    }

    [[nodiscard]] constexpr auto lifecycle() const noexcept
        -> ObjectLifecycle {
        return lifecycle_;
    }

private:
    struct Ops final {
        bool (*try_ref)(void*, ObjectAnchor&, u64) noexcept;
        void (*drop_ref)(void*, ObjectAnchor&, u64) noexcept;
        void* (*try_pin)(void*, ObjectAnchor&, u64) noexcept;
        void (*drop_pin)(void*, ObjectAnchor&, u64) noexcept;
        bool (*request_retire)(void*, ObjectAnchor&, u64) noexcept;
        void (*finish_cleanup)(void*, ObjectAnchor&, u64) noexcept;
    };

    template<typename T>
    friend class ObjectPool;
    template<typename T>
    friend class ObjectPin;
    friend class ObjectRef;
    friend class ObjectCleanup;
    friend class kernel::resource::Reservation;
    friend class kernel::resource::Permit;
    friend class kernel::resource::Refund;
    friend class kernel::resource::Charge;
    friend class kernel::resource::Sponsorship;
    friend class kernel::resource::ResourcePool;

    void* owner_{};
    const Ops* ops_{};
    u64 generation_{};
    usize strong_refs_{};
    usize active_pins_{};
    ObjectKind kind_{ObjectKind::Invalid};
    ObjectLifecycle lifecycle_{ObjectLifecycle::Free};
    bool cleanup_complete_{};
    bool reclaim_queued_{};
    kernel::resource::Sponsorship sponsorship_{};
};

} // namespace kernel::object
