#include <object/object_store.hpp>

#include <core/debug.hpp>

namespace kernel::object {

ObjectStore::ObjectStore(
    kernel::mm::Pmm& pmm,
    kernel::mm::VSpaceExecutor& vspace_work) noexcept
    : pmm_(&pmm),
      vspace_work_(&vspace_work),
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
    KASSERT(cspaces_.live_count() == 0);
    KASSERT(memories_.live_count() == 0);
    KASSERT(domains_.live_count() == 0);
    KASSERT(contexts_.live_count() == 0);
    KASSERT(threads_.live_count() == 0);
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
    auto pending = memories_.create(*pmm_, byte_size);
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

auto ObjectStore::create_physical(
    usize byte_size,
    libk::Span<const kernel::mm::MemoryExtent> extents) noexcept
    -> libk::Expected<MemoryPending, kernel::mm::MemoryError> {
    auto pending = memories_.create(*pmm_, byte_size);
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
    auto pending = memories_.create(*pmm_, byte_size);
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

auto ObjectStore::hold_vspace(ObjectId id) noexcept
    -> libk::Expected<VSpaceHold, VSpacePool::Error> {
    return vspaces_.hold(id);
}

auto ObjectStore::pin_vspace(ObjectId id) noexcept
    -> libk::Expected<VSpacePin, VSpacePool::Error> {
    return vspaces_.pin(id);
}

void ObjectStore::drain_reclaim() noexcept {
    vspaces_.drain_reclaim();
    cspaces_.drain_reclaim();
    memories_.drain_reclaim();
    domains_.drain_reclaim();
    contexts_.drain_reclaim();
    threads_.drain_reclaim();
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
