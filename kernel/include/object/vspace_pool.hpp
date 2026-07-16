#pragma once

#include <object/object_pool.hpp>
#include <mm/vspace.hpp>

namespace kernel::object {

template<>
struct ObjectTraits<kernel::mm::VSpace> final {
    static constexpr ObjectKind kind = ObjectKind::VSpace;

    [[nodiscard]] static auto prepare_retire(kernel::mm::VSpace& space) noexcept
        -> bool {
        return space.prepare_retire();
    }
    static void retire(
        kernel::mm::VSpace& space,
        ObjectCleanup&& cleanup) noexcept {
        space.retire(libk::move(cleanup));
    }
    static void destroy(kernel::mm::VSpace& space) noexcept {
        libk::destroy_at(&space);
    }
};

using VSpacePool = ObjectPool<kernel::mm::VSpace>;
using VSpacePending = VSpacePool::Pending;
using VSpaceHold = VSpacePool::Hold;
using VSpacePin = VSpacePool::Pin;

} // namespace kernel::object
