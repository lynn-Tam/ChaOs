#pragma once

#include <io/space.hpp>
#include <object/pool.hpp>

namespace io {

// Capability storage has no controller, discovery or machine lifetime policy.
class objects final {
public:
    objects(mm::Pmm& pmm, libk::delegate<void() noexcept>& notify) noexcept
        : irqs(pmm, notify), spaces(pmm, notify), devices(pmm, notify) {}
    auto drain() noexcept -> usize {
        return spaces.drain_reclaim() + irqs.drain_reclaim() + devices.drain_reclaim();
    }
    object::pool<irq::Irq> irqs;
    object::pool<Space> spaces;
    object::pool<Device> devices;
};

} // namespace io
