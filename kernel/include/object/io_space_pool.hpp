#pragma once

#include <io/space.hpp>
#include <object/object_pool.hpp>

namespace kernel::object {
template<>
struct ObjectTraits<io::Space> final {
    static constexpr ObjectKind kind = ObjectKind::IoSpace;
    static void bind_sponsor(io::Space& space, resource::Sponsorship& sponsor) noexcept {
        space.bind_sponsor(sponsor);
    }
    static void retire(io::Space& space, ObjectCleanup&& cleanup) noexcept {
        space.retire(libk::move(cleanup));
    }
    static void destroy(io::Space& space) noexcept { libk::destroy_at(&space); }
};
using IoSpacePool = ObjectPool<io::Space>;
using IoSpacePending = IoSpacePool::Pending;
using IoSpaceHold = IoSpacePool::Hold;
using IoSpacePin = IoSpacePool::Pin;
} // namespace kernel::object
