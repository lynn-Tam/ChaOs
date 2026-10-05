#pragma once

#include <object/pool.hpp>
#include <ipc/channel.hpp>
#include <ipc/endpoint.hpp>
#include <ipc/notification.hpp>

namespace ipc {

struct objects {
    using notifier = libk::delegate<void() noexcept>;
    objects(mm::Pmm& pmm, notifier& notify) noexcept
        : endpoints(pmm, notify), channels(pmm, notify),
          notifications(pmm, notify) {}
    auto drain() noexcept -> usize {
        auto count = endpoints.drain_reclaim();
        count += channels.drain_reclaim();
        return count + notifications.drain_reclaim();
    }
    object::pool<Endpoint> endpoints;
    object::pool<Channel> channels;
    object::pool<Notification> notifications;
};

} // namespace ipc
