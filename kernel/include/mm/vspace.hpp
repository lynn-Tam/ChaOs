#pragma once

#include <arch/page_editor.hpp>
#include <arch/page_table.hpp>
#include <cap/authority.hpp>
#include <cap/handle.hpp>
#include <core/types.hpp>
#include <libk/expected.hpp>
#include <libk/sync/atomic.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/manual_lifetime.hpp>
#include <libk/noncopyable.hpp>
#include <libk/sync/ticket_spin_lock.hpp>
#include <libk/utility.hpp>
#include <mm/address_region.hpp>
#include <mm/kernel_vspace.hpp>
#include <mm/user_view.hpp>
#include <mm/mapping_authority.hpp>
#include <mm/node_pool.hpp>
#include <mm/translation.hpp>
#include <object/object_cleanup.hpp>
#include <resource/sponsorship.hpp>

namespace kernel {
class CpuRegistry;
class ExecutionBinding;
namespace object {
template<typename T>
struct ObjectTraits;
}
namespace cap {
class CSpace;
template<typename T>
class Resolved;
}
}

namespace kernel::mm {

class VSpaceExecutor;

enum class VSpaceState : u8 {
    Building,
    Live,
    Stopping,
    Quiescent,
};

enum class VSpaceError : u8 {
    InvalidState,
    InvalidRange,
    InvalidRegion,
    InvalidMapping,
    InvalidAuthority,
    InvalidAccess,
    Overlap,
    NotMapped,
    Busy,
    OutOfMemory,
    QuotaExceeded,
    GenerationExhausted,
    BackingFailed,
    UnsupportedMemoryType,
    AliasConflict,
    GrantUnavailable,
    ShootdownUnavailable,
    TranslationCorrupt,
    ResourceExhausted,
};

enum class VmStatus : u8 {
    Complete,
    Pending,
};

enum class VSpaceServiceState : u8 {
    Settled,
    Progress,
    Waiting,
    Retry,
};

enum class VSpaceServiceError : u8 {
    ResourceExhausted,
    BackingFailed,
    TranslationCorrupt,
    InvariantViolation,
};

using VSpaceServiceResult =
    libk::Expected<VSpaceServiceState, VSpaceServiceError>;

struct VmContext final {
    kernel::CpuRegistry* cpus{};
    kernel::CpuId local{};
};

struct MapRequest final {
    VirtRange virtual_range{};
    ObjectRange object{};
    AccessMask access{};
};

struct MapResult final {
    MappingKey mapping{};
    VmStatus status{VmStatus::Complete};
};

struct RegionCapResult final {
    RegionKey region{};
    kernel::cap::CapHandle capability{};
};

enum class FaultKind : u8 {
    NoMapping,
    Guard,
    AccessDenied,
    Busy,
    BackingFailed,
    Ready,
    Materialized,
};

struct FaultResult final {
    FaultKind kind{FaultKind::NoMapping};
    MappingKey mapping{};
    usize object_page{};
    VmStatus status{VmStatus::Complete};
};

struct MappingInfo final {
    MappingKey key{};
    RegionKey region{};
    VirtRange range{};
    ObjectRange object{};
    AccessMask access{};
    AccessMask ceiling{};
    MemoryTypes types{};
    MappingState state{MappingState::Detached};
    AuthoritySource source{AuthoritySource::Kernel};
};

class VSpace final : private libk::noncopyable_nonmovable {
    using InvalidationList = libk::IntrusiveList<
        MappingAuthority, &MappingAuthority::invalidation_hook_>;

public:
    VSpace(Pmm& pmm, KernelVSpace& kernel, VSpaceExecutor& work) noexcept;
    ~VSpace() noexcept;

    [[nodiscard]] auto initialize() noexcept
        -> libk::Expected<void, VSpaceError>;

    [[nodiscard]] auto state() const noexcept -> VSpaceState;
    [[nodiscard]] auto root_key() const noexcept -> RegionKey;
    [[nodiscard]] auto translation() noexcept -> TranslationView;
    [[nodiscard]] auto active_cpus() const noexcept -> kernel::CpuSet {
        return coherence_.active_cpus();
    }
    [[nodiscard]] auto binding_count() const noexcept -> usize;

    // Binds a trusted kernel consumer to the unique live Mapping covering the
    // requested virtual/object ranges. MappingKey remains VSpace-internal;
    // callers cannot manufacture a second mapping identity truth.
    [[nodiscard]] auto bind_view(UserViewRequest&& request) noexcept
        -> libk::Expected<UserView, VSpaceError>;

    [[nodiscard]] auto create_region(
        RegionKey parent,
        VirtRange range,
        RegionPolicy policy) noexcept
        -> libk::Expected<RegionKey, VSpaceError>;
    [[nodiscard]] auto create_region(
        kernel::cap::Resolved<VSpace>& source,
        kernel::cap::CSpace& destination,
        VirtRange range,
        RegionPolicy policy,
        kernel::cap::Rights child_rights) noexcept
        -> libk::Expected<RegionCapResult, VSpaceError>;
    [[nodiscard]] auto reserve(
        RegionKey parent,
        VirtRange range) noexcept -> libk::Expected<void, VSpaceError>;
    [[nodiscard]] auto guard(
        RegionKey parent,
        VirtRange range) noexcept -> libk::Expected<void, VSpaceError>;

    [[nodiscard]] auto map_kernel(
        VmContext context,
        RegionKey region,
        MapRequest request,
        object::ObjectRef&& memory,
        MemoryObject& object,
        cap::MemoryAuthority authority) noexcept
        -> libk::Expected<MapResult, VSpaceError>;

    [[nodiscard]] auto map(
        VmContext context,
        cap::VSpaceAuthority where,
        MapRequest request,
        kernel::cap::Resolved<MemoryObject>& memory) noexcept
        -> libk::Expected<MapResult, VSpaceError>;

    [[nodiscard]] auto unmap(
        VmContext context,
        cap::VSpaceAuthority where,
        VirtRange range) noexcept -> libk::Expected<VmStatus, VSpaceError>;
    [[nodiscard]] auto unmap_kernel(
        VmContext context,
        RegionKey region,
        VirtRange range) noexcept -> libk::Expected<VmStatus, VSpaceError>;
    [[nodiscard]] auto protect(
        VmContext context,
        cap::VSpaceAuthority where,
        VirtRange range,
        AccessMask access) noexcept
        -> libk::Expected<VmStatus, VSpaceError>;
    [[nodiscard]] auto protect_kernel(
        VmContext context,
        RegionKey region,
        VirtRange range,
        AccessMask access) noexcept
        -> libk::Expected<VmStatus, VSpaceError>;
    [[nodiscard]] auto destroy_region(
        VmContext context,
        RegionKey region) noexcept
        -> libk::Expected<VmStatus, VSpaceError>;

    [[nodiscard]] auto fault(
        VmContext context,
        VirtAddr address,
        Access access) noexcept -> libk::Expected<FaultResult, VSpaceError>;
    [[nodiscard]] auto inspect(MappingKey key) const noexcept
        -> libk::Expected<MappingInfo, VSpaceError>;

    // Completes remote shootdowns and queued Memory/Grant invalidations.
    // Background continuation has a distinct retry/wait/fatal contract and
    // never leaks syscall-facing VSpaceError values to its executor.
    [[nodiscard]] auto service(VmContext context) noexcept
        -> VSpaceServiceResult;
    [[nodiscard]] auto pending() const noexcept -> bool;

    void retire(object::ObjectCleanup&& cleanup) noexcept;

private:
    friend class VSpaceExecutor;
    friend class MappingAuthority;
    friend class kernel::ExecutionBinding;
    friend class UserView;
    friend struct kernel::object::ObjectTraits<VSpace>;

    enum class PendingKind : u8 {
        None,
        Map,
        Unmap,
        Protect,
        Invalidate,
        DestroyRegion,
        Retire,
    };

    struct RangeClaim final {
        AddressRegion* region{};
        VirtRange range{};
    };

    // Transaction-local table capacity.  charge is declared before pages so
    // physical ownership is released first during reverse destruction.
    struct TableReserve final {
        TableReserve(
            kernel::resource::Charge&& table_charge,
            OwnedPageGroup&& table_pages) noexcept
            : charge(libk::move(table_charge)),
              pages(libk::move(table_pages)) {}

        TableReserve(TableReserve&&) noexcept = default;
        auto operator=(TableReserve&&) noexcept -> TableReserve& = default;

        kernel::resource::Charge charge{};
        OwnedPageGroup pages{};
    };

    [[nodiscard]] auto map_impl(
        VmContext context,
        AddressRegion& region,
        MapRequest request,
        object::ObjectRef&& memory_ref,
        MemoryObject& memory,
        cap::MemoryAuthority memory_authority,
        AccessMask vspace_access,
        MemoryTypes vspace_types,
        kernel::cap::Resolved<MemoryObject>* capability) noexcept
        -> libk::Expected<MapResult, VSpaceError>;
    [[nodiscard]] auto unmap_impl(
        VmContext context,
        AddressRegion& region,
        VirtRange range) noexcept -> libk::Expected<VmStatus, VSpaceError>;
    [[nodiscard]] auto protect_impl(
        VmContext context,
        AddressRegion& region,
        VirtRange range,
        AccessMask access) noexcept
        -> libk::Expected<VmStatus, VSpaceError>;

    [[nodiscard]] auto validate_region(
        RegionKey key,
        VirtRange range) noexcept -> AddressRegion*;
    [[nodiscard]] auto validate_region(
        cap::VSpaceAuthority authority,
        VirtRange range) noexcept -> AddressRegion*;
    [[nodiscard]] static auto valid_user_range(VirtRange range) noexcept
        -> bool;
    [[nodiscard]] static auto overlap(
        AddressRegion& region,
        VirtRange range) noexcept -> LayoutNode*;
    [[nodiscard]] auto prepare_plan(
        VmContext context,
        const kernel::CpuSet& targets) noexcept
        -> libk::Expected<ShootdownPlan, VSpaceError>;
    [[nodiscard]] auto begin_claim(
        AddressRegion& region,
        VirtRange range,
        bool must_be_empty) noexcept -> libk::Expected<void, VSpaceError>;
    void release_claim() noexcept;

    [[nodiscard]] auto commit_translation(
        TranslationState::Mutation&& mutation,
        ShootdownPlan&& plan,
        RetireBatch& retire,
        bool instruction_sync = false) noexcept
        -> libk::Expected<VmStatus, VSpaceError>;
    [[nodiscard]] auto finish_pending() noexcept -> bool;
    void queue_layout(LayoutNode& node) noexcept;
    void queue_page(MappedPage& page) noexcept;
    void queue_authority(MappingAuthority& authority) noexcept;
    void destroy_layout(LayoutNode& node) noexcept;
    void detach_mapping(Mapping& mapping) noexcept;
    void release_page(MappedPage& page) noexcept;
    void finish_authorities() noexcept;

    void request_invalidation(
        MappingAuthority& authority,
        MemoryWork&& work) noexcept;
    void request_invalidation(
        MappingAuthority& authority,
        cap::GrantWork&& work) noexcept;
    [[nodiscard]] auto start_invalidation(
        VmContext context,
        MappingAuthority& authority,
        PendingKind kind = PendingKind::Invalidate) noexcept
        -> libk::Expected<VmStatus, VSpaceError>;

    [[nodiscard]] auto split_for_unmap(
        Mapping& source,
        VirtRange removed,
        Mapping*& first,
        Mapping*& second) noexcept -> libk::Expected<void, VSpaceError>;
    [[nodiscard]] auto make_fragment(
        Mapping& source,
        VirtRange range,
        AccessMask access) noexcept
        -> libk::Expected<Mapping*, VSpaceError>;

    [[nodiscard]] auto materialize_fault(
        VmContext context,
        Mapping& mapping,
        VirtAddr page_address,
        usize object_page) noexcept
        -> libk::Expected<FaultResult, VSpaceError>;
    [[nodiscard]] auto reserve_tables(
        MappedPage* pages) noexcept
        -> libk::Expected<TableReserve, VSpaceError>;
    void commit_tables(TableReserve& reserve) noexcept;
    void retire_table(RetireBatch& retire, OwnedPage&& page) noexcept;
    void release_root() noexcept;

    [[nodiscard]] auto start_region_destroy(
        VmContext context,
        AddressRegion& region,
        bool remove_root,
        PendingKind kind) noexcept
        -> libk::Expected<VmStatus, VSpaceError>;
    void dismantle_region(
        AddressRegion& region,
        arch::PageEditor& editor,
        RetireBatch& retire,
        bool remove_root) noexcept;
    void try_finish_retire() noexcept;
    void complete_cleanup() noexcept;
    void translation_ready() noexcept;
    void schedule_work() noexcept;
    [[nodiscard]] auto work_ready() const noexcept -> bool;
    [[nodiscard]] auto prepare_retire() noexcept -> bool;
    [[nodiscard]] auto attach_execution() noexcept -> bool;
    void detach_execution() noexcept;
    void bind_sponsor(kernel::resource::Sponsorship& sponsor) noexcept;
    void detach_view(UserViewRelation& relation) noexcept;
    [[nodiscard]] auto view_active(const UserViewRelation& relation) const noexcept
        -> bool;
    void invalidate_views(Mapping& mapping) noexcept;

    Pmm* pmm_{};
    KernelVSpace* kernel_{};
    VSpaceExecutor* work_{};
    mutable libk::TicketSpinLock lock_{};
    libk::ManualLifetime<arch::UserRoot> root_{};
    TranslationState coherence_{};
    NodePool<AddressRegion> regions_;
    NodePool<Mapping> mappings_;
    NodePool<ReservedLeaf> reservations_;
    NodePool<Guard> guards_;
    NodePool<MappingAuthority> authorities_;
    NodePool<MappedPage> pages_;
    NodePool<UserViewRelation> views_;
    AddressRegion* root_region_{};
    RangeClaim claim_{};
    InvalidationList invalidations_{};
    PendingKind pending_kind_{PendingKind::None};
    LayoutNode* pending_layout_{};
    LayoutNode* pending_protected_{};
    MappedPage* pending_pages_{};
    MappingAuthority* pending_authorities_{};
    libk::ManualLifetime<ShootdownTicket> ticket_{};
    libk::ManualLifetime<RetireBatch> retire_batch_{};
    libk::ManualLifetime<object::ObjectCleanup> cleanup_{};
    libk::IntrusiveListHook work_hook_{};
    libk::Atomic<bool> work_open_{false};
    VSpaceState state_{VSpaceState::Building};
    usize bindings_{};
    usize transport_retries_{};
    bool service_waiting_on_claim_{};
    kernel::resource::Sponsorship* sponsor_{};
    kernel::resource::Charge table_charge_{};
};

} // namespace kernel::mm
