#pragma once

#include <libk/memory.hpp>
#include <object/object_pool.hpp>
#include <object/object_id.hpp>
#include <object/object_traits.hpp>
#include <sched/context.hpp>
#include <sched/domain.hpp>

namespace kernel::object {

template<>
struct ObjectTraits<kernel::sched::SchedulingContext> final {
    static constexpr ObjectKind kind = ObjectKind::SchedulingContext;

    static void retire(
        [[maybe_unused]] kernel::sched::SchedulingContext& context) noexcept {
    }

    static void destroy(kernel::sched::SchedulingContext& context) noexcept {
        libk::destroy_at(&context);
    }
};

template<>
struct ObjectTraits<kernel::sched::SchedulingDomain> final {
    static constexpr ObjectKind kind = ObjectKind::SchedulingDomain;

    static void retire(
        [[maybe_unused]] kernel::sched::SchedulingDomain& domain) noexcept {
    }

    static void destroy(kernel::sched::SchedulingDomain& domain) noexcept {
        libk::destroy_at(&domain);
    }
};

using SchedulingContextPool = ObjectPool<kernel::sched::SchedulingContext>;
using SchedulingContextPending = SchedulingContextPool::Pending;
using SchedulingContextHold = SchedulingContextPool::Hold;
using SchedulingContextPin = SchedulingContextPool::Pin;

using SchedulingDomainPool = ObjectPool<kernel::sched::SchedulingDomain>;
using SchedulingDomainPending = SchedulingDomainPool::Pending;
using SchedulingDomainHold = SchedulingDomainPool::Hold;
using SchedulingDomainPin = SchedulingDomainPool::Pin;

} // namespace kernel::object
