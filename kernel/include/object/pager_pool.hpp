#pragma once

#include <libk/memory.hpp>
#include <object/object_pool.hpp>
#include <pager/pager.hpp>

namespace kernel::object {

template<>
struct ObjectTraits<kernel::pager::Pager> final {
    static constexpr ObjectKind kind = ObjectKind::Pager;

    static void retire(
        kernel::pager::Pager& pager,
        ObjectCleanup&& cleanup) noexcept {
        pager.retire(libk::move(cleanup));
    }
    static void destroy(kernel::pager::Pager& pager) noexcept {
        libk::destroy_at(&pager);
    }
};

using PagerPool = ObjectPool<kernel::pager::Pager>;
using PagerPending = PagerPool::Pending;
using PagerHold = PagerPool::Hold;
using PagerPin = PagerPool::Pin;

} // namespace kernel::object
