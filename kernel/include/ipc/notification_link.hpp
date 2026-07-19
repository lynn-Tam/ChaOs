#pragma once

namespace kernel::ipc {

class Notification;

// Non-owning reverse index embedded in Notification. The Notification-owned
// receiver relation keeps the target Vproc structurally alive while linked.
struct NotificationLink final {
    explicit NotificationLink(Notification& owner) noexcept
        : notification(&owner) {}

    Notification* notification{};
};

} // namespace kernel::ipc
