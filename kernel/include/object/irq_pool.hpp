#pragma once

#include <irq/irq.hpp>
#include <libk/memory.hpp>
#include <object/object_pool.hpp>

namespace kernel::object {

template<>
struct ObjectTraits<kernel::irq::Irq> final {
    static constexpr ObjectKind kind = ObjectKind::Irq;

    static void retire(
        kernel::irq::Irq& irq,
        ObjectCleanup&& cleanup) noexcept {
        irq.retire(libk::move(cleanup));
    }
    static void destroy(kernel::irq::Irq& irq) noexcept {
        libk::destroy_at(&irq);
    }
};

using IrqPool = ObjectPool<kernel::irq::Irq>;
using IrqPending = IrqPool::Pending;
using IrqHold = IrqPool::Hold;
using IrqPin = IrqPool::Pin;

} // namespace kernel::object
