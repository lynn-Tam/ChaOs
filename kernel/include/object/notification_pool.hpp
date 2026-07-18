#pragma once

#include <ipc/notification.hpp>
#include <libk/memory.hpp>
#include <object/object_pool.hpp>

namespace kernel::object {

template<>
struct ObjectTraits<kernel::ipc::Notification> final {
    static constexpr ObjectKind kind = ObjectKind::Notification;

    static void retire(
        kernel::ipc::Notification& notification,
        ObjectCleanup&& cleanup) noexcept {
        notification.retire(libk::move(cleanup));
    }
    static void destroy(kernel::ipc::Notification& notification) noexcept {
        libk::destroy_at(&notification);
    }
};

using NotificationPool = ObjectPool<kernel::ipc::Notification>;
using NotificationPending = NotificationPool::Pending;
using NotificationHold = NotificationPool::Hold;
using NotificationPin = NotificationPool::Pin;

} // namespace kernel::object
