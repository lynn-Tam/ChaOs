#pragma once

#include <libk/delegate.hpp>
#include <libk/expected.hpp>
#include <libk/noncopyable.hpp>
#include <libk/utility.hpp>
#include <object/cspace_pool.hpp>
#include <object/memory_pool.hpp>
#include <object/sched_pool.hpp>
#include <object/thread_pool.hpp>
#include <object/vspace_pool.hpp>

namespace kernel::object {

// Typed construction and lookup façade for capability-addressable kernel
// objects. ObjectPool<T> owns storage/lifecycle mechanics; ObjectStore owns the
// concrete pool set and is the sole cross-type reclaim boundary.
class ObjectStore final : private libk::noncopyable_nonmovable {
public:
    using ReclaimNotifier = libk::delegate<void() noexcept>;
    using ThreadPool = object::ThreadPool;
    using ThreadPending = object::ThreadPending;
    using ThreadHold = object::ThreadHold;
    using ThreadPin = object::ThreadPin;
    using SchedulingContextPool = object::SchedulingContextPool;
    using SchedulingContextPending = object::SchedulingContextPending;
    using SchedulingContextHold = object::SchedulingContextHold;
    using SchedulingContextPin = object::SchedulingContextPin;
    using SchedulingDomainPool = object::SchedulingDomainPool;
    using SchedulingDomainPending = object::SchedulingDomainPending;
    using SchedulingDomainHold = object::SchedulingDomainHold;
    using SchedulingDomainPin = object::SchedulingDomainPin;
    using CSpacePool = object::CSpacePool;
    using CSpacePending = object::CSpacePending;
    using CSpaceHold = object::CSpaceHold;
    using CSpacePin = object::CSpacePin;
    using MemoryPool = object::MemoryPool;
    using MemoryPending = object::MemoryPending;
    using MemoryHold = object::MemoryHold;
    using MemoryPin = object::MemoryPin;
    using VSpacePool = object::VSpacePool;
    using VSpacePending = object::VSpacePending;
    using VSpaceHold = object::VSpaceHold;
    using VSpacePin = object::VSpacePin;

    explicit ObjectStore(kernel::mm::Pmm& pmm, kernel::mm::VSpaceExecutor& vspace_work) noexcept;
    ~ObjectStore() noexcept;

    template<typename... Args>
    [[nodiscard]] auto create_thread(Args&&... args) noexcept
        -> libk::Expected<ThreadPending, ThreadPool::Error> {
        return threads_.create(libk::forward<Args>(args)...);
    }

    template<typename... Args>
    [[nodiscard]] auto create_context(Args&&... args) noexcept
        -> libk::Expected<
            SchedulingContextPending,
            SchedulingContextPool::Error> {
        return contexts_.create(libk::forward<Args>(args)...);
    }

    template<typename... Args>
    [[nodiscard]] auto create_domain(Args&&... args) noexcept
        -> libk::Expected<
            SchedulingDomainPending,
            SchedulingDomainPool::Error> {
        return domains_.create(libk::forward<Args>(args)...);
    }

    [[nodiscard]] auto hold_thread(ObjectId id) noexcept
        -> libk::Expected<ThreadHold, ThreadPool::Error>;
    [[nodiscard]] auto pin_thread(ObjectId id) noexcept
        -> libk::Expected<ThreadPin, ThreadPool::Error>;
    [[nodiscard]] auto hold_context(ObjectId id) noexcept
        -> libk::Expected<SchedulingContextHold, SchedulingContextPool::Error>;
    [[nodiscard]] auto pin_context(ObjectId id) noexcept
        -> libk::Expected<SchedulingContextPin, SchedulingContextPool::Error>;
    [[nodiscard]] auto hold_domain(ObjectId id) noexcept
        -> libk::Expected<SchedulingDomainHold, SchedulingDomainPool::Error>;
    [[nodiscard]] auto pin_domain(ObjectId id) noexcept
        -> libk::Expected<SchedulingDomainPin, SchedulingDomainPool::Error>;

    [[nodiscard]] auto create_cspace() noexcept
        -> libk::Expected<CSpacePending, CSpacePool::Error>;
    [[nodiscard]] auto create_cspace(cap::CSpace::Quota quota) noexcept
        -> libk::Expected<CSpacePending, CSpacePool::Error>;
    [[nodiscard]] auto hold_cspace(ObjectId id) noexcept
        -> libk::Expected<CSpaceHold, CSpacePool::Error>;
    [[nodiscard]] auto pin_cspace(ObjectId id) noexcept
        -> libk::Expected<CSpacePin, CSpacePool::Error>;

    [[nodiscard]] auto create_anonymous(
        usize byte_size,
        kernel::mm::AnonymousConfig config = {}) noexcept
        -> libk::Expected<MemoryPending, kernel::mm::MemoryError>;
    [[nodiscard]] auto create_physical(
        usize byte_size,
        libk::Span<const kernel::mm::MemoryExtent> extents) noexcept
        -> libk::Expected<MemoryPending, kernel::mm::MemoryError>;
    [[nodiscard]] auto create_boot_image(
        usize byte_size,
        libk::Span<const kernel::mm::MemoryExtent> extents,
        kernel::mm::BootOwnership ownership,
        kernel::mm::OwnedPageGroup&& pages = {}) noexcept
        -> libk::Expected<MemoryPending, kernel::mm::MemoryError>;
    [[nodiscard]] auto hold_memory(ObjectId id) noexcept
        -> libk::Expected<MemoryHold, MemoryPool::Error>;
    [[nodiscard]] auto pin_memory(ObjectId id) noexcept
        -> libk::Expected<MemoryPin, MemoryPool::Error>;

    [[nodiscard]] auto create_vspace(kernel::mm::KernelVSpace& kernel) noexcept
        -> libk::Expected<VSpacePending, kernel::mm::VSpaceError>;
    [[nodiscard]] auto hold_vspace(ObjectId id) noexcept
        -> libk::Expected<VSpaceHold, VSpacePool::Error>;
    [[nodiscard]] auto pin_vspace(ObjectId id) noexcept
        -> libk::Expected<VSpacePin, VSpacePool::Error>;

    // Runtime has exactly one drain executor. Tests and terminal teardown may
    // call this directly only while that executor is absent or stopped.
    void drain_reclaim() noexcept;
    void bind_reclaim_notifier(ReclaimNotifier notifier) noexcept;
    void unbind_reclaim_notifier() noexcept;

private:
    [[nodiscard]] static auto memory_pool_error(ObjectError error) noexcept
        -> kernel::mm::MemoryError;
    [[nodiscard]] static auto vspace_pool_error(ObjectError error) noexcept
        -> kernel::mm::VSpaceError;

    // The callback is a scheduling hint only. Pool reclaim lists remain the
    // canonical work state, so a notifier failure cannot create a second queue.
    ReclaimNotifier reclaim_notify_{};
    kernel::mm::Pmm* pmm_{};
    kernel::mm::VSpaceExecutor* vspace_work_{};
    ThreadPool threads_;
    SchedulingContextPool contexts_;
    SchedulingDomainPool domains_;
    CSpacePool cspaces_;
    MemoryPool memories_;
    VSpacePool vspaces_;
};

} // namespace kernel::object
