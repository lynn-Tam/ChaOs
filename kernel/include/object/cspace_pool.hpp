#pragma once

#include <cap/cspace.hpp>
#include <object/object_pool.hpp>

namespace kernel::object {

template<>
struct ObjectTraits<kernel::cap::CSpace> final {
    static constexpr ObjectKind kind = ObjectKind::CSpace;

    static void bind_sponsor(
        kernel::cap::CSpace& space,
        kernel::resource::Sponsorship& sponsor) noexcept {
        space.bind_sponsor(sponsor);
    }

    [[nodiscard]] static auto prepare_retire(
        kernel::cap::CSpace& space) noexcept -> bool {
        return space.prepare_retire();
    }
    static void retire(kernel::cap::CSpace& space) noexcept {
        space.retire();
    }
    static void destroy(kernel::cap::CSpace& space) noexcept {
        libk::destroy_at(&space);
    }
};

using CSpacePool = ObjectPool<kernel::cap::CSpace>;
using CSpacePending = CSpacePool::Pending;
using CSpaceHold = CSpacePool::Hold;
using CSpacePin = CSpacePool::Pin;

} // namespace kernel::object
