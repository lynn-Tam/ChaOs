#pragma once

#include <object/object_pool.hpp>
#include <resource/pool.hpp>

namespace kernel::object {

template<>
struct ObjectTraits<kernel::resource::ResourcePool> final {
    static constexpr ObjectKind kind = ObjectKind::ResourcePool;

    [[nodiscard]] static auto prepare_retire(
        kernel::resource::ResourcePool& pool) noexcept -> bool {
        return pool.can_retire();
    }
    static void retire(
        [[maybe_unused]] kernel::resource::ResourcePool& pool) noexcept {}
    static void destroy(kernel::resource::ResourcePool& pool) noexcept {
        libk::destroy_at(&pool);
    }
};

using ResourcePool = ObjectPool<kernel::resource::ResourcePool>;
using ResourcePending = ResourcePool::Pending;
using ResourceHold = ResourcePool::Hold;
using ResourcePin = ResourcePool::Pin;

} // namespace kernel::object
