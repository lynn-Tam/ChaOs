#pragma once

#include <object/pool.hpp>
#include <object/group.hpp>
#include <task/thread.hpp>

struct Tasks {
    using notifier = libk::delegate<void() noexcept>;
    Tasks(mm::Pmm& pmm, notifier& notify) noexcept
        : groups(pmm, notify), threads(pmm, notify) {}
    auto drain() noexcept -> usize {
        auto count = threads.drain_reclaim();
        return count + groups.drain_reclaim();
    }
    // Sponsoring groups outlive all objects whose slots they pay for.
    object::pool<object::group> groups;
    object::pool<Thread> threads;
};

