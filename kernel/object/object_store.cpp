#include <object/object_store.hpp>

#include <core/debug.hpp>

namespace kernel::object {

/*luna change: require the shared reclaimer in ObjectStore construction,
  reason: every Pager backing uses one explicit membership owner*/
ObjectStore::ObjectStore(
    kernel::mm::Pmm& pmm,
    kernel::mm::VSpaceExecutor& vspace_work,
    kernel::mm::MemoryExecutor& memory_work,
    kernel::mm::PageReclaimer& reclaimer) noexcept
    : pmm_(&pmm),
      vspace_work_(&vspace_work),
      memory_work_(&memory_work),
      reclaimer_(reclaimer),
      resources_(pmm, reclaim_notify_),
      endpoints_(pmm, reclaim_notify_),
      channels_(pmm, reclaim_notify_),
      pagers_(pmm, reclaim_notify_),
      irqs_(pmm, reclaim_notify_),
      tunnels_(pmm, reclaim_notify_),
      vprocs_(pmm, reclaim_notify_),
      notifications_(pmm, reclaim_notify_),
      threads_(pmm, reclaim_notify_),
      contexts_(pmm, reclaim_notify_),
      domains_(pmm, reclaim_notify_),
      cspaces_(pmm, reclaim_notify_),
      memories_(pmm, reclaim_notify_),
      vspaces_(pmm, reclaim_notify_) {}

ObjectStore::~ObjectStore() noexcept {
    KASSERT(!reclaim_notify_);
    drain_reclaim();
    KASSERT(vspaces_.live_count() == 0);
    KASSERT(endpoints_.live_count() == 0);
    KASSERT(channels_.live_count() == 0);
    KASSERT(pagers_.live_count() == 0);
    KASSERT(irqs_.live_count() == 0);
    KASSERT(tunnels_.live_count() == 0);
    KASSERT(vprocs_.live_count() == 0);
    KASSERT(notifications_.live_count() == 0);
    KASSERT(cspaces_.live_count() == 0);
    KASSERT(memories_.live_count() == 0);
    KASSERT(domains_.live_count() == 0);
    KASSERT(contexts_.live_count() == 0);
    KASSERT(threads_.live_count() == 0);
    KASSERT(resources_.live_count() == 0);
}

auto ObjectStore::hold_thread(ObjectId id) noexcept
    -> libk::Expected<ThreadHold, ThreadPool::Error> {
    return threads_.hold(id);
}

auto ObjectStore::pin_thread(ObjectId id) noexcept
    -> libk::Expected<ThreadPin, ThreadPool::Error> {
    return threads_.pin(id);
}

auto ObjectStore::hold_context(ObjectId id) noexcept
    -> libk::Expected<SchedulingContextHold, SchedulingContextPool::Error> {
    return contexts_.hold(id);
}

auto ObjectStore::pin_context(ObjectId id) noexcept
    -> libk::Expected<SchedulingContextPin, SchedulingContextPool::Error> {
    return contexts_.pin(id);
}

auto ObjectStore::hold_domain(ObjectId id) noexcept
    -> libk::Expected<SchedulingDomainHold, SchedulingDomainPool::Error> {
    return domains_.hold(id);
}

auto ObjectStore::pin_domain(ObjectId id) noexcept
    -> libk::Expected<SchedulingDomainPin, SchedulingDomainPool::Error> {
    return domains_.pin(id);
}

auto ObjectStore::create_cspace() noexcept
    -> libk::Expected<CSpacePending, CSpacePool::Error> {
    return cspaces_.create(*pmm_);
}

auto ObjectStore::create_cspace(cap::CSpace::Quota quota) noexcept
    -> libk::Expected<CSpacePending, CSpacePool::Error> {
    return cspaces_.create(*pmm_, quota);
}

auto ObjectStore::create_cspace_sponsored(
    kernel::resource::Reservation&& sponsorship,
    cap::CSpace::Quota quota) noexcept
    -> libk::Expected<CSpacePending, CSpacePool::Error> {
    return cspaces_.create_sponsored(
        libk::move(sponsorship), *pmm_, quota);
}

auto ObjectStore::hold_cspace(ObjectId id) noexcept
    -> libk::Expected<CSpaceHold, CSpacePool::Error> {
    return cspaces_.hold(id);
}

auto ObjectStore::pin_cspace(ObjectId id) noexcept
    -> libk::Expected<CSpacePin, CSpacePool::Error> {
    return cspaces_.pin(id);
}

auto ObjectStore::create_anonymous(
    usize byte_size,
    kernel::mm::AnonymousConfig config) noexcept
    -> libk::Expected<MemoryPending, kernel::mm::MemoryError> {
    /*luna change: thread the shared reclaimer through every memory pool
      creation, reason: construction must preserve one Pager policy owner*/
    auto pending = memories_.create(
        *pmm_, byte_size, *memory_work_, reclaimer_);
    if (!pending) {
        return libk::unexpected(memory_pool_error(pending.error()));
    }
    MemoryPending memory = libk::move(pending).value();
    auto initialized = memory.get().initialize_anonymous(config);
    if (!initialized) {
        return libk::unexpected(initialized.error());
    }
    return libk::expected(libk::move(memory));
}

auto ObjectStore::create_anonymous_sponsored(
    kernel::resource::Reservation&& sponsorship,
    usize byte_size,
    kernel::mm::AnonymousConfig config) noexcept
    -> libk::Expected<MemoryPending, kernel::mm::MemoryError> {
    auto pending = memories_.create_sponsored(
        libk::move(sponsorship), *pmm_, byte_size, *memory_work_, reclaimer_);
    if (!pending) {
        return libk::unexpected(memory_pool_error(pending.error()));
    }
    MemoryPending memory = libk::move(pending).value();
    auto initialized = memory.get().initialize_anonymous(config);
    if (!initialized) {
        return libk::unexpected(initialized.error());
    }
    return libk::expected(libk::move(memory));
}

auto ObjectStore::create_pager_memory_sponsored(
    kernel::resource::Reservation&& sponsorship,
    usize byte_size,
    object::ObjectRef&& pager,
    kernel::mm::AccessMask access) noexcept
    -> libk::Expected<MemoryPending, kernel::mm::MemoryError> {
    auto pending = memories_.create_sponsored(
        libk::move(sponsorship), *pmm_, byte_size, *memory_work_, reclaimer_);
    if (!pending) {
        return libk::unexpected(memory_pool_error(pending.error()));
    }
    MemoryPending memory = libk::move(pending).value();
    auto initialized = memory.get().initialize_pager(
        libk::move(pager), access);
    if (!initialized) {
        return libk::unexpected(initialized.error());
    }
    return libk::expected(libk::move(memory));
}

auto ObjectStore::create_physical(
    usize byte_size,
    libk::Span<const kernel::mm::MemoryExtent> extents) noexcept
    -> libk::Expected<MemoryPending, kernel::mm::MemoryError> {
    auto pending = memories_.create(
        *pmm_, byte_size, *memory_work_, reclaimer_);
    if (!pending) {
        return libk::unexpected(memory_pool_error(pending.error()));
    }
    MemoryPending memory = libk::move(pending).value();
    auto initialized = memory.get().initialize_physical(extents);
    if (!initialized) {
        return libk::unexpected(initialized.error());
    }
    return libk::expected(libk::move(memory));
}

auto ObjectStore::create_physical_sponsored(
    kernel::resource::Reservation&& sponsorship,
    usize byte_size,
    libk::Span<const kernel::mm::MemoryExtent> extents) noexcept
    -> libk::Expected<MemoryPending, kernel::mm::MemoryError> {
    auto pending = memories_.create_sponsored(
        libk::move(sponsorship), *pmm_, byte_size, *memory_work_, reclaimer_);
    if (!pending) {
        return libk::unexpected(memory_pool_error(pending.error()));
    }
    MemoryPending memory = libk::move(pending).value();
    auto initialized = memory.get().initialize_physical(extents);
    if (!initialized) {
        return libk::unexpected(initialized.error());
    }
    return libk::expected(libk::move(memory));
}

auto ObjectStore::create_boot_image(
    usize byte_size,
    libk::Span<const kernel::mm::MemoryExtent> extents,
    kernel::mm::BootOwnership ownership,
    kernel::mm::OwnedPageGroup&& pages) noexcept
    -> libk::Expected<MemoryPending, kernel::mm::MemoryError> {
    auto pending = memories_.create(
        *pmm_, byte_size, *memory_work_, reclaimer_);
    if (!pending) {
        return libk::unexpected(memory_pool_error(pending.error()));
    }
    MemoryPending memory = libk::move(pending).value();
    auto initialized = memory.get().initialize_boot_image(
        extents, ownership, libk::move(pages));
    if (!initialized) {
        return libk::unexpected(initialized.error());
    }
    return libk::expected(libk::move(memory));
}

auto ObjectStore::create_boot_image_sponsored(
    kernel::resource::Reservation&& sponsorship,
    usize byte_size,
    libk::Span<const kernel::mm::MemoryExtent> extents,
    kernel::mm::BootOwnership ownership,
    kernel::mm::OwnedPageGroup&& pages) noexcept
    -> libk::Expected<MemoryPending, kernel::mm::MemoryError> {
    auto pending = memories_.create_sponsored(
        libk::move(sponsorship), *pmm_, byte_size, *memory_work_, reclaimer_);
    if (!pending) {
        return libk::unexpected(memory_pool_error(pending.error()));
    }
    MemoryPending memory = libk::move(pending).value();
    auto initialized = memory.get().initialize_boot_image(
        extents, ownership, libk::move(pages));
    if (!initialized) {
        return libk::unexpected(initialized.error());
    }
    return libk::expected(libk::move(memory));
}

auto ObjectStore::hold_memory(ObjectId id) noexcept
    -> libk::Expected<MemoryHold, MemoryPool::Error> {
    return memories_.hold(id);
}

auto ObjectStore::pin_memory(ObjectId id) noexcept
    -> libk::Expected<MemoryPin, MemoryPool::Error> {
    return memories_.pin(id);
}

auto ObjectStore::create_vspace(kernel::mm::KernelVSpace& kernel) noexcept
    -> libk::Expected<VSpacePending, kernel::mm::VSpaceError> {
    auto pending = vspaces_.create(*pmm_, kernel, *vspace_work_);
    if (!pending) {
        return libk::unexpected(vspace_pool_error(pending.error()));
    }
    VSpacePending space = libk::move(pending).value();
    auto initialized = space.get().initialize();
    if (!initialized) {
        return libk::unexpected(initialized.error());
    }
    return libk::expected(libk::move(space));
}

auto ObjectStore::create_vspace_sponsored(
    kernel::resource::Reservation&& sponsorship,
    kernel::mm::KernelVSpace& kernel) noexcept
    -> libk::Expected<VSpacePending, kernel::mm::VSpaceError> {
    auto pending = vspaces_.create_sponsored(
        libk::move(sponsorship), *pmm_, kernel, *vspace_work_);
    if (!pending) {
        return libk::unexpected(vspace_pool_error(pending.error()));
    }
    VSpacePending space = libk::move(pending).value();
    auto initialized = space.get().initialize();
    if (!initialized) {
        return libk::unexpected(initialized.error());
    }
    return libk::expected(libk::move(space));
}

auto ObjectStore::hold_vspace(ObjectId id) noexcept
    -> libk::Expected<VSpaceHold, VSpacePool::Error> {
    return vspaces_.hold(id);
}

auto ObjectStore::pin_vspace(ObjectId id) noexcept
    -> libk::Expected<VSpacePin, VSpacePool::Error> {
    return vspaces_.pin(id);
}

auto ObjectStore::create_resource(kernel::resource::Budget limit) noexcept
    -> libk::Expected<ResourcePending, ResourcePool::Error> {
    return resources_.create(limit);
}

auto ObjectStore::create_resource_sponsored(
    kernel::resource::Reservation&& sponsorship,
    kernel::resource::Budget limit) noexcept
    -> libk::Expected<ResourcePending, ResourcePool::Error> {
    return resources_.create_sponsored(
        libk::move(sponsorship), limit);
}

auto ObjectStore::hold_resource(ObjectId id) noexcept
    -> libk::Expected<ResourceHold, ResourcePool::Error> {
    return resources_.hold(id);
}

auto ObjectStore::pin_resource(ObjectId id) noexcept
    -> libk::Expected<ResourcePin, ResourcePool::Error> {
    return resources_.pin(id);
}

auto ObjectStore::create_notification_sponsored(
    kernel::resource::Reservation&& sponsorship) noexcept
    -> libk::Expected<NotificationPending, NotificationPool::Error> {
    return notifications_.create_sponsored(libk::move(sponsorship));
}

auto ObjectStore::hold_notification(ObjectId id) noexcept
    -> libk::Expected<NotificationHold, NotificationPool::Error> {
    return notifications_.hold(id);
}

auto ObjectStore::pin_notification(ObjectId id) noexcept
    -> libk::Expected<NotificationPin, NotificationPool::Error> {
    return notifications_.pin(id);
}

auto ObjectStore::hold_vproc(ObjectId id) noexcept
    -> libk::Expected<VprocHold, VprocPool::Error> {
    return vprocs_.hold(id);
}

auto ObjectStore::pin_vproc(ObjectId id) noexcept
    -> libk::Expected<VprocPin, VprocPool::Error> {
    return vprocs_.pin(id);
}

auto ObjectStore::hold_tunnel(ObjectId id) noexcept
    -> libk::Expected<TunnelHold, TunnelPool::Error> {
    return tunnels_.hold(id);
}

auto ObjectStore::pin_tunnel(ObjectId id) noexcept
    -> libk::Expected<TunnelPin, TunnelPool::Error> {
    return tunnels_.pin(id);
}

auto ObjectStore::hold_endpoint(ObjectId id) noexcept
    -> libk::Expected<EndpointHold, EndpointPool::Error> {
    return endpoints_.hold(id);
}

auto ObjectStore::pin_endpoint(ObjectId id) noexcept
    -> libk::Expected<EndpointPin, EndpointPool::Error> {
    return endpoints_.pin(id);
}

auto ObjectStore::hold_channel(ObjectId id) noexcept
    -> libk::Expected<ChannelHold, ChannelPool::Error> {
    return channels_.hold(id);
}

auto ObjectStore::pin_channel(ObjectId id) noexcept
    -> libk::Expected<ChannelPin, ChannelPool::Error> {
    return channels_.pin(id);
}

auto ObjectStore::hold_pager(ObjectId id) noexcept
    -> libk::Expected<PagerHold, PagerPool::Error> {
    return pagers_.hold(id);
}

auto ObjectStore::pin_pager(ObjectId id) noexcept
    -> libk::Expected<PagerPin, PagerPool::Error> {
    return pagers_.pin(id);
}

auto ObjectStore::hold_irq(ObjectId id) noexcept
    -> libk::Expected<IrqHold, IrqPool::Error> {
    return irqs_.hold(id);
}

auto ObjectStore::pin_irq(ObjectId id) noexcept
    -> libk::Expected<IrqPin, IrqPool::Error> {
    return irqs_.pin(id);
}

auto ObjectStore::drain_reclaim() noexcept -> usize {
    usize drained{};
    drained += endpoints_.drain_reclaim();
    drained += channels_.drain_reclaim();
    drained += pagers_.drain_reclaim();
    drained += irqs_.drain_reclaim();
    drained += tunnels_.drain_reclaim();
    drained += vprocs_.drain_reclaim();
    drained += notifications_.drain_reclaim();
    drained += vspaces_.drain_reclaim();
    drained += cspaces_.drain_reclaim();
    drained += memories_.drain_reclaim();
    drained += domains_.drain_reclaim();
    drained += contexts_.drain_reclaim();
    drained += threads_.drain_reclaim();
    drained += resources_.drain_reclaim();
    return drained;
}

void ObjectStore::bind_reclaim_notifier(ReclaimNotifier notifier) noexcept {
    KASSERT(notifier);
    KASSERT(!reclaim_notify_);
    reclaim_notify_ = notifier;
}

void ObjectStore::unbind_reclaim_notifier() noexcept {
    reclaim_notify_.reset();
}

auto ObjectStore::memory_pool_error(ObjectError error) noexcept
    -> kernel::mm::MemoryError {
    return error == ObjectError::GenerationExhausted
        ? kernel::mm::MemoryError::GenerationExhausted
        : kernel::mm::MemoryError::OutOfMemory;
}

auto ObjectStore::vspace_pool_error(ObjectError error) noexcept
    -> kernel::mm::VSpaceError {
    return error == ObjectError::GenerationExhausted
        ? kernel::mm::VSpaceError::GenerationExhausted
        : kernel::mm::VSpaceError::OutOfMemory;
}

} // namespace kernel::object
