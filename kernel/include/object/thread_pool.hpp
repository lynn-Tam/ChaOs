#pragma once

#include <libk/memory.hpp>
#include <object/object_pool.hpp>
#include <object/object_id.hpp>
#include <object/object_traits.hpp>
#include <thread/thread.hpp>

namespace kernel::object {

template<>
struct ObjectTraits<kernel::Thread> final {
    static constexpr ObjectKind kind = ObjectKind::Thread;

    [[nodiscard]] static auto prepare_retire(
        const kernel::Thread& thread) noexcept -> bool {
        return thread.prepare_retire();
    }

    static void retire([[maybe_unused]] kernel::Thread& thread) noexcept {
    }

    static void destroy(kernel::Thread& thread) noexcept {
        libk::destroy_at(&thread);
    }
};

using ThreadPool = ObjectPool<kernel::Thread>;
using ThreadPending = ThreadPool::Pending;
using ThreadHold = ThreadPool::Hold;
using ThreadPin = ThreadPool::Pin;

} // namespace kernel::object
