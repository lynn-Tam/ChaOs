#pragma once

#include <mm/memory_object.hpp>
#include <object/object_pool.hpp>

namespace kernel::object {

template<>
struct ObjectTraits<kernel::mm::MemoryObject> final {
    static constexpr ObjectKind kind = ObjectKind::MemoryObject;

    static void retire(kernel::mm::MemoryObject& memory) noexcept {
        memory.retire();
    }
    static void destroy(kernel::mm::MemoryObject& memory) noexcept {
        libk::destroy_at(&memory);
    }
};

using MemoryPool = ObjectPool<kernel::mm::MemoryObject>;
using MemoryPending = MemoryPool::Pending;
using MemoryHold = MemoryPool::Hold;
using MemoryPin = MemoryPool::Pin;

} // namespace kernel::object
