#pragma once

#include <object/pool.hpp>
#include <sched/sc.hpp>
#include <sched/domain.hpp>

namespace sched {

struct objects {
    using notifier = libk::delegate<void() noexcept>;
    objects(mm::Pmm& pmm, notifier& notify) noexcept
        : contexts(pmm, notify), domains(pmm, notify) {}
    auto drain() noexcept -> usize {
        const auto count = domains.drain_reclaim();
        return count + contexts.drain_reclaim();
    }
    object::pool<Sc> contexts;
    object::pool<Domain> domains;
};

} // namespace sched
