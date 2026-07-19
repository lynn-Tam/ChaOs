#pragma once

#include <libk/delegate.hpp>
#include <libk/expected.hpp>
#include <libk/noncopyable.hpp>
#include <libk/utility.hpp>
#include <object/cspace_pool.hpp>
#include <object/endpoint_pool.hpp>
#include <object/memory_pool.hpp>
#include <object/notification_pool.hpp>
#include <object/resource_pool.hpp>
#include <object/sched_pool.hpp>
#include <object/thread_pool.hpp>
#include <object/tunnel_pool.hpp>
#include <object/vspace_pool.hpp>
#include <object/vproc_pool.hpp>

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
    using ResourcePool = object::ResourcePool;
    using ResourcePending = object::ResourcePending;
    using ResourceHold = object::ResourceHold;
    using ResourcePin = object::ResourcePin;
    using NotificationPool = object::NotificationPool;
    using NotificationPending = object::NotificationPending;
    using NotificationHold = object::NotificationHold;
    using NotificationPin = object::NotificationPin;
    using VprocPool = object::VprocPool;
    using VprocPending = object::VprocPending;
    using VprocHold = object::VprocHold;
    using VprocPin = object::VprocPin;
    using TunnelPool = object::TunnelPool;
    using TunnelPending = object::TunnelPending;
    using TunnelHold = object::TunnelHold;
    using TunnelPin = object::TunnelPin;
    using EndpointPool = object::EndpointPool;
    using EndpointPending = object::EndpointPending;
    using EndpointHold = object::EndpointHold;
    using EndpointPin = object::EndpointPin;

    explicit ObjectStore(kernel::mm::Pmm& pmm, kernel::mm::VSpaceExecutor& vspace_work) noexcept;
    ~ObjectStore() noexcept;

    template<typename... Args>
    [[nodiscard]] auto create_thread(Args&&... args) noexcept
        -> libk::Expected<ThreadPending, ThreadPool::Error> {
        return threads_.create(libk::forward<Args>(args)...);
    }

    template<typename... Args>
    [[nodiscard]] auto create_thread_sponsored(
        kernel::resource::Reservation&& sponsorship,
        Args&&... args) noexcept
        -> libk::Expected<ThreadPending, ThreadPool::Error> {
        return threads_.create_sponsored(
            libk::move(sponsorship), libk::forward<Args>(args)...);
    }

    template<typename... Args>
    [[nodiscard]] auto create_context(Args&&... args) noexcept
        -> libk::Expected<
            SchedulingContextPending,
            SchedulingContextPool::Error> {
        return contexts_.create(libk::forward<Args>(args)...);
    }

    template<typename... Args>
    [[nodiscard]] auto create_context_sponsored(
        kernel::resource::Reservation&& sponsorship,
        Args&&... args) noexcept
        -> libk::Expected<
            SchedulingContextPending,
            SchedulingContextPool::Error> {
        return contexts_.create_sponsored(
            libk::move(sponsorship), libk::forward<Args>(args)...);
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
    [[nodiscard]] auto create_cspace_sponsored(
        kernel::resource::Reservation&& sponsorship,
        cap::CSpace::Quota quota = {}) noexcept
        -> libk::Expected<CSpacePending, CSpacePool::Error>;
    [[nodiscard]] auto hold_cspace(ObjectId id) noexcept
        -> libk::Expected<CSpaceHold, CSpacePool::Error>;
    [[nodiscard]] auto pin_cspace(ObjectId id) noexcept
        -> libk::Expected<CSpacePin, CSpacePool::Error>;

    [[nodiscard]] auto create_anonymous(
        usize byte_size,
        kernel::mm::AnonymousConfig config = {}) noexcept
        -> libk::Expected<MemoryPending, kernel::mm::MemoryError>;
    [[nodiscard]] auto create_anonymous_sponsored(
        kernel::resource::Reservation&& sponsorship,
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
    [[nodiscard]] auto create_boot_image_sponsored(
        kernel::resource::Reservation&& sponsorship,
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
    [[nodiscard]] auto create_vspace_sponsored(
        kernel::resource::Reservation&& sponsorship,
        kernel::mm::KernelVSpace& kernel) noexcept
        -> libk::Expected<VSpacePending, kernel::mm::VSpaceError>;
    [[nodiscard]] auto hold_vspace(ObjectId id) noexcept
        -> libk::Expected<VSpaceHold, VSpacePool::Error>;
    [[nodiscard]] auto pin_vspace(ObjectId id) noexcept
        -> libk::Expected<VSpacePin, VSpacePool::Error>;

    [[nodiscard]] auto create_resource(kernel::resource::Budget limit) noexcept
        -> libk::Expected<ResourcePending, ResourcePool::Error>;
    [[nodiscard]] auto create_resource_sponsored(
        kernel::resource::Reservation&& sponsorship,
        kernel::resource::Budget limit) noexcept
        -> libk::Expected<ResourcePending, ResourcePool::Error>;
    [[nodiscard]] auto hold_resource(ObjectId id) noexcept
        -> libk::Expected<ResourceHold, ResourcePool::Error>;
    [[nodiscard]] auto pin_resource(ObjectId id) noexcept
        -> libk::Expected<ResourcePin, ResourcePool::Error>;

    [[nodiscard]] auto create_notification_sponsored(
        kernel::resource::Reservation&& sponsorship) noexcept
        -> libk::Expected<NotificationPending, NotificationPool::Error>;
    [[nodiscard]] auto hold_notification(ObjectId id) noexcept
        -> libk::Expected<NotificationHold, NotificationPool::Error>;
    [[nodiscard]] auto pin_notification(ObjectId id) noexcept
        -> libk::Expected<NotificationPin, NotificationPool::Error>;

    template<typename... Args>
    [[nodiscard]] auto create_vproc_sponsored(
        kernel::resource::Reservation&& sponsorship,
        Args&&... args) noexcept
        -> libk::Expected<VprocPending, VprocPool::Error> {
        return vprocs_.create_sponsored(
            libk::move(sponsorship), libk::forward<Args>(args)...);
    }
    [[nodiscard]] auto hold_vproc(ObjectId id) noexcept
        -> libk::Expected<VprocHold, VprocPool::Error>;
    [[nodiscard]] auto pin_vproc(ObjectId id) noexcept
        -> libk::Expected<VprocPin, VprocPool::Error>;

    template<typename... Args>
    [[nodiscard]] auto create_tunnel_sponsored(
        kernel::resource::Reservation&& sponsorship,
        Args&&... args) noexcept
        -> libk::Expected<TunnelPending, TunnelPool::Error> {
        return tunnels_.create_sponsored(
            libk::move(sponsorship), libk::forward<Args>(args)...);
    }
    [[nodiscard]] auto hold_tunnel(ObjectId id) noexcept
        -> libk::Expected<TunnelHold, TunnelPool::Error>;
    [[nodiscard]] auto pin_tunnel(ObjectId id) noexcept
        -> libk::Expected<TunnelPin, TunnelPool::Error>;

    template<typename... Args>
    [[nodiscard]] auto create_endpoint_sponsored(
        kernel::resource::Reservation&& sponsorship,
        Args&&... args) noexcept
        -> libk::Expected<EndpointPending, EndpointPool::Error> {
        return endpoints_.create_sponsored(
            libk::move(sponsorship), *pmm_, libk::forward<Args>(args)...);
    }
    [[nodiscard]] auto hold_endpoint(ObjectId id) noexcept
        -> libk::Expected<EndpointHold, EndpointPool::Error>;
    [[nodiscard]] auto pin_endpoint(ObjectId id) noexcept
        -> libk::Expected<EndpointPin, EndpointPool::Error>;

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
    // Sponsoring pools outlive every pool containing sponsored objects.
    ResourcePool resources_;
    EndpointPool endpoints_;
    TunnelPool tunnels_;
    VprocPool vprocs_;
    NotificationPool notifications_;
    ThreadPool threads_;
    SchedulingContextPool contexts_;
    SchedulingDomainPool domains_;
    CSpacePool cspaces_;
    MemoryPool memories_;
    VSpacePool vspaces_;
};

} // namespace kernel::object
