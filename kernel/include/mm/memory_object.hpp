#pragma once

#include <core/types.hpp>
#include <libk/expected.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/noncopyable.hpp>
#include <libk/span.hpp>
#include <libk/sync/atomic.hpp>
#include <sync/lock.hpp>
#include <mm/pmm.hpp>
#include <mm/page_state.hpp>
#include <mm/reclaim.hpp>
#include <mm/object_range.hpp>
#include <mm/permissions.hpp>
#include <object/object_ref.hpp>
#include <resource/sponsorship.hpp>

namespace kernel::object {
template<typename T>
struct ObjectTraits;
}

namespace kernel {
class Vproc;
}

namespace kernel::pager {
class Pager;
struct ClaimKey;
}

namespace kernel::operation {
class PageFault;
}

namespace kernel::mm {

class MemoryExecutor;

/*luna change: return bounded backing progress as a transport result, reason: executor scheduling must not duplicate PageSlot semantic state*/
struct MemoryServiceBatch final {
    usize processed{};
    usize progressed{};
    bool more{};
};

struct MemoryExtent final {
    ObjectRange object{};
    PageRange physical{};
    AccessMask access{};
    MemoryType type{MemoryType::Normal};
};

struct AnonymousConfig final {
    AccessMask access{AccessMask::of(Access::Read, Access::Write)};
    bool eager{};
};

enum class BootOwnership : u8 {
    Borrowed,
    Owned,
};

enum class BackingKind : u8 {
    Anonymous,
    Physical,
    BootImage,
    Pager,
};

enum class ContentState : u8 {
    Zero,
    Resident,
    Busy,
    Failed,
};

struct MemoryPage final {
    Page page{};
    AccessMask access{};
    MemoryType type{MemoryType::Normal};
};

class MemoryObject;
class MemoryWork;

// A materialized PTE is a reverse relation of the MemoryObject page. The
// mapping owns this node; the backing only indexes it while the mapping is
// live. It carries no page ownership or PTE state.
/*luna change: add one exact reverse-relation claim state, reason: reclaim
  invalidates a single PTE without destroying MappingAuthority layout*/
class PageMapping final : private libk::noncopyable_nonmovable {
public:
    using Publish = void (*)(void*, MemoryWork&&) noexcept;

    PageMapping() noexcept = default;
    ~PageMapping() noexcept;

    [[nodiscard]] auto attached() const noexcept -> bool {
        return owner_ != nullptr;
    }
    [[nodiscard]] auto page_index() const noexcept -> usize {
        return page_index_;
    }
    [[nodiscard]] auto owner() const noexcept -> MemoryObject* {
        return owner_;
    }
    /*luna change: expose only relation-state checks needed by exact teardown,
      reason: token ownership is canonical in PageMapping state and context*/
    [[nodiscard]] auto invalidating() const noexcept -> bool {
        return static_cast<State>(state_.load<libk::MemoryOrder::Acquire>())
            == State::Invalidating;
    }
    [[nodiscard]] auto owns(const MemoryWork& work) const noexcept -> bool;

    void arm(void* context, Publish publish) noexcept;
    [[nodiscard]] auto claim() noexcept -> MemoryWork;
    void publish(MemoryWork&& work) noexcept;
    void mark_attached() noexcept;
    void mark_detached() noexcept;

    // Public only so an owner-local intrusive index can name the hook.
    libk::IntrusiveListHook backing_hook_{};

private:
    friend class MemoryObject;

    enum class State : u8 {
        Detached,
        Attached,
        Invalidating,
    };

    static void release_work(void* context) noexcept;
    void finish_work() noexcept;

    MemoryObject* owner_{};
    usize page_index_{};
    void* context_{};
    Publish publish_{};
    libk::Atomic<u8> state_{static_cast<u8>(State::Detached)};
};

enum class MemoryError : u8 {
    InvalidSize,
    InvalidRange,
    InvalidAccess,
    InvalidMemoryType,
    InvalidState,
    OutOfMemory,
    ResourceExhausted,
    Pressure,
    GenerationExhausted,
    Busy,
    Pending,
    BackingFailed,
    NotBacked,
    AttachmentState,
    OwnershipMismatch,
};

enum class MemoryInvalidation : u8 {
    Destroy,
};

enum class MemoryState : u8 {
    Building,
    Live,
    Stopping,
    Retired,
};

enum class SealState : u8 {
    Loadable,
    Sealing,
    Executable,
};

struct ContentEpoch final {
    u64 raw{};

    [[nodiscard]] friend constexpr auto operator==(
        ContentEpoch, ContentEpoch) noexcept -> bool = default;
};

class MemoryAttachment;

// A source page remains owned by its staging MemoryObject until the target
// pager backing accepts it.  Destruction aborts the transfer and restores the
// source slot, so a failed supply cannot leave two owners or no owner.
class PageTransfer final : private libk::noncopyable {
public:
    PageTransfer() noexcept = default;
    PageTransfer(PageTransfer&& other) noexcept;
    auto operator=(PageTransfer&& other) noexcept -> PageTransfer&;
    ~PageTransfer() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept {
        return owner_ != nullptr;
    }
    [[nodiscard]] auto page() const noexcept -> Page {
        return page_.page();
    }
    [[nodiscard]] auto take_page() noexcept -> OwnedPage {
        return libk::move(page_);
    }
    void commit() noexcept;
    void abort() noexcept;

private:
    friend class MemoryObject;
    PageTransfer(
        MemoryObject& owner,
        usize index,
        OwnedPage&& page) noexcept
        : owner_(&owner), index_(index), page_(libk::move(page)) {}

    MemoryObject* owner_{};
    usize index_{};
    OwnedPage page_{};
};

/*luna change: make MemoryWork a move-only exact-release token, reason:
  MemoryAttachment and PageMapping share lifetime release without a parallel lease*/
class MemoryWork final : private libk::noncopyable {
public:
    MemoryWork() noexcept = default;
    MemoryWork(MemoryWork&& other) noexcept;
    auto operator=(MemoryWork&& other) noexcept -> MemoryWork&;
    ~MemoryWork() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept {
        return context_ != nullptr && release_ != nullptr;
    }
    void reset() noexcept;

private:
    friend class MemoryObject;
    friend class MemoryAttachment;
    friend class PageMapping;

    using Release = void (*)(void*) noexcept;
    MemoryWork(void* context, Release release) noexcept
        : context_(context), release_(release) {}
    explicit MemoryWork(MemoryAttachment& attachment) noexcept;

    void* context_{};
    Release release_{};
};

struct MemoryAttachmentOps final {
    void (*invalidate)(
        void* context,
        MemoryWork&& work,
        MemoryInvalidation reason) noexcept;
    void (*released)(void* context) noexcept;
};

// Embedded in a Mapping and indexed non-owningly by MemoryObject. The Mapping
// must retain its structural MemoryObject reference until detach() completes.
class MemoryAttachment final : private libk::noncopyable_nonmovable {
public:
    MemoryAttachment(
        void* context,
        const MemoryAttachmentOps& ops) noexcept
        : context_(context), ops_(&ops) {}
    ~MemoryAttachment() noexcept;

    [[nodiscard]] auto attached() const noexcept -> bool;
    [[nodiscard]] auto busy() const noexcept -> bool;
    // Returns true when no MemoryWork still pins the Mapping relation.
    [[nodiscard]] auto detach() noexcept -> bool;

private:
    friend class MemoryObject;
    friend class MemoryWork;

    enum class State : u8 {
        Idle,
        Attached,
        Invalidating,
        Detached,
    };

    void drop_work() noexcept;

    libk::IntrusiveListHook memory_hook_{};
    MemoryObject* owner_{};
    void* context_{};
    const MemoryAttachmentOps* ops_{};
    libk::Atomic<usize> work_{};
    libk::Atomic<u8> state_{static_cast<u8>(State::Idle)};
    AccessMask access_{};
};

// Short backing borrow. Callers keep the MemoryObject operation pin or a
// structural Mapping reference alive for the complete lease lifetime.
class PageLease final : private libk::noncopyable {
public:
    PageLease() noexcept = default;
    PageLease(PageLease&& other) noexcept;
    auto operator=(PageLease&& other) noexcept -> PageLease&;
    ~PageLease() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept {
        return owner_ != nullptr;
    }
    [[nodiscard]] auto page() const noexcept -> MemoryPage { return page_; }
    void reset() noexcept;

private:
    friend class MemoryObject;
    PageLease(MemoryObject& owner, MemoryPage page) noexcept
        : owner_(&owner), page_(page) {}

    MemoryObject* owner_{};
    MemoryPage page_{};
};

class MemoryObject final : private libk::noncopyable_nonmovable {
public:
    /*luna change: require the kernel-private reclaim policy owner, reason:
      every MemoryObject shares one explicit Pager membership boundary*/
    MemoryObject(
        Pmm& pmm,
        usize byte_size,
        MemoryExecutor& work,
        PageReclaimer& reclaimer) noexcept;
    ~MemoryObject() noexcept;

    [[nodiscard]] auto initialize_anonymous(AnonymousConfig config) noexcept
        -> libk::Expected<void, MemoryError>;
    [[nodiscard]] auto initialize_physical(
        libk::Span<const MemoryExtent> extents) noexcept
        -> libk::Expected<void, MemoryError>;
    [[nodiscard]] auto initialize_boot_image(
        libk::Span<const MemoryExtent> extents,
        BootOwnership ownership,
        OwnedPageGroup&& owned = {}) noexcept
        -> libk::Expected<void, MemoryError>;
    [[nodiscard]] auto initialize_pager(
        kernel::pager::Pager& pager,
        AccessMask access) noexcept
        -> libk::Expected<void, MemoryError>;
    // The structural reference keeps the Pager payload alive for the whole
    // backing lifetime. The raw overload above is retained for stack-owned
    // unit-test pagers and is intentionally non-owning.
    [[nodiscard]] auto initialize_pager(
        object::ObjectRef&& pager,
        AccessMask access) noexcept
        -> libk::Expected<void, MemoryError>;

    [[nodiscard]] auto size() const noexcept -> usize {
        return logical_pages_ * page_size;
    }
    [[nodiscard]] auto page_count() const noexcept -> usize {
        return logical_pages_;
    }
    [[nodiscard]] auto kind() const noexcept -> BackingKind;
    [[nodiscard]] auto state() const noexcept -> MemoryState;
    [[nodiscard]] auto seal_state() const noexcept -> SealState;
    [[nodiscard]] auto content_epoch() const noexcept -> ContentEpoch;
    // Publishes immutable executable content. The synchronous E0 path only
    // succeeds once no writable mapping attachment remains; later async
    // callers use the same state transition after retiring those mappings.
    [[nodiscard]] auto seal() noexcept -> libk::Expected<void, MemoryError>;
    [[nodiscard]] auto query(usize page_index) const noexcept
        -> libk::Expected<ContentState, MemoryError>;
    [[nodiscard]] auto materialize(usize page_index) noexcept
        -> libk::Expected<PageLease, MemoryError>;
    /*luna change: retain the existing operation pin while a caller relation is attached, reason: Thread cancellation must own one exact MemoryObject lifetime without a parallel lease*/
    [[nodiscard]] auto materialize(
        usize page_index,
        WaitRelation* relation,
        void* owner,
        WaitRelation::Publish publish,
        FrameDemand* demand = nullptr) noexcept
        -> libk::Expected<PageLease, MemoryError>;
    [[nodiscard]] auto begin_transfer(usize page_index) noexcept
        -> libk::Expected<PageTransfer, MemoryError>;
    [[nodiscard]] auto pager_supply(
        pager::Pager& pager,
        usize page_index,
        PageKey page_key,
        pager::ClaimKey claim,
        OwnedPage&& page,
        u64 content_epoch) noexcept
        -> libk::Expected<void, MemoryError>;
    [[nodiscard]] auto pager_supply_transfer(
        pager::Pager& pager,
        PageTransfer&& transfer,
        usize page_index,
        PageKey page_key,
        pager::ClaimKey claim,
        u64 content_epoch) noexcept
        -> libk::Expected<void, MemoryError>;
    [[nodiscard]] auto pager_fail(
        pager::Pager& pager,
        usize page_index,
        PageKey page_key,
        pager::ClaimKey claim) noexcept
        -> libk::Expected<void, MemoryError>;
    [[nodiscard]] auto bind_mapping(
        PageMapping& mapping,
        usize page_index) noexcept -> libk::Expected<void, MemoryError>;
    [[nodiscard]] auto unbind_mapping(
        PageMapping& mapping) noexcept -> libk::Expected<void, MemoryError>;
    /*luna change: finish a claimed unlink with its exact relation token,
      reason: backing hook removal must precede token release*/
    [[nodiscard]] auto unbind_mapping(
        PageMapping& mapping,
        MemoryWork& work) noexcept -> libk::Expected<void, MemoryError>;
    /*luna change: expose the existing exact relation claim to bounded
      reclaimer service, reason: callers publish immediately after unlock*/
    [[nodiscard]] auto claim_mapping(PageMapping& mapping) noexcept
        -> libk::Expected<MemoryWork, MemoryError>;
    [[nodiscard]] auto page_state(usize page_index) const noexcept
        -> libk::Expected<PageSlotState, MemoryError>;
    [[nodiscard]] auto observe_usage(
        usize page_index,
        bool accessed,
        bool dirty) noexcept -> libk::Expected<void, MemoryError>;
    [[nodiscard]] auto drain_page_waiters(
        usize capacity) noexcept -> usize;
    [[nodiscard]] auto queue_writeback(usize page_index) noexcept
        -> libk::Expected<WritebackKey, MemoryError>;
    [[nodiscard]] auto publish_writeback(
        usize page_index,
        WritebackKey key) noexcept
        -> libk::Expected<void, MemoryError>;
    [[nodiscard]] auto complete_writeback(
        pager::Pager& pager,
        usize page_index,
        WritebackKey key,
        u64 delivery_generation,
        u64 claim_generation) noexcept -> libk::Expected<void, MemoryError>;
    [[nodiscard]] auto fail_writeback(
        pager::Pager& pager,
        usize page_index,
        WritebackKey key,
        u64 delivery_generation,
        u64 claim_generation,
        WritebackFailure failure) noexcept
        -> libk::Expected<void, MemoryError>;
    [[nodiscard]] auto evict_page(usize page_index) noexcept
        -> libk::Expected<void, MemoryError>;
    [[nodiscard]] auto read(usize offset, libk::Span<byte> output) noexcept
        -> libk::Expected<void, MemoryError>;

    [[nodiscard]] auto attach(
        MemoryAttachment& attachment,
        AccessMask access) noexcept
        -> libk::Expected<void, MemoryError>;
    [[nodiscard]] auto attachment_count() const noexcept -> usize;
    void retire() noexcept;
private:
    friend struct kernel::object::ObjectTraits<MemoryObject>;
    friend class PageLease;
    friend class PageTransfer;
    friend class MemoryAttachment;
    friend class MemoryExecutor;
    friend class PageReclaimer;
    friend class PagerBacking;
    friend class kernel::operation::PageFault;
    /*luna change: let the fixed Vproc continuation settle the existing fault pin, reason: Vproc shares MemoryObject lifetime ownership without a second lease API*/
    friend class kernel::Vproc;

    using AttachmentList = libk::IntrusiveList<
        MemoryAttachment,
        &MemoryAttachment::memory_hook_>;

    struct BackingOps final {
        BackingKind kind;
        ContentState (*query)(const void* backing, usize page_index) noexcept;
        libk::Expected<MemoryPage, MemoryError> (*materialize)(
            void* backing,
            usize page_index,
            FrameDemand* demand,
            WaitRelation* relation,
            void* owner,
            WaitRelation::Publish publish) noexcept;
        bool (*cancel_fault)(
            void* backing,
            WaitRelation& relation,
            u64 generation) noexcept;
        libk::Expected<OwnedPage, MemoryError> (*begin_transfer)(
            void* backing,
            usize page_index) noexcept;
        libk::Expected<void, MemoryError> (*restore_transfer)(
            void* backing,
            usize page_index,
            OwnedPage&& page) noexcept;
        libk::Expected<void, MemoryError> (*commit_transfer)(
            void* backing,
            usize page_index) noexcept;
        libk::Expected<void, MemoryError> (*supply)(
            void* backing,
            pager::Pager& pager,
            usize page_index,
            PageKey page_key,
            pager::ClaimKey claim,
            OwnedPage&& page,
            u64 content_epoch) noexcept;
        libk::Expected<void, MemoryError> (*fail)(
            void* backing,
            pager::Pager& pager,
            usize page_index,
            PageKey page_key,
            pager::ClaimKey claim) noexcept;
        libk::Expected<void, MemoryError> (*bind_mapping)(
            void* backing,
            usize page_index,
            PageMapping& mapping) noexcept;
        libk::Expected<void, MemoryError> (*unbind_mapping)(
            void* backing,
            PageMapping& mapping) noexcept;
        /*luna change: expose exact claimed unlink separately from normal
          teardown, reason: Invalidating mappings reject a non-token unbind*/
        libk::Expected<void, MemoryError> (*unbind_claimed_mapping)(
            void* backing,
            PageMapping& mapping,
            MemoryWork& work) noexcept;
        /*luna change: expose one exact mapping claim hook, reason: backing
          ownership selects a relation before VSpace performs foreign work*/
        libk::Expected<MemoryWork, MemoryError> (*claim_mapping)(
            void* backing,
            PageMapping& mapping) noexcept;
        libk::Expected<void, MemoryError> (*lease_acquire)(
            void* backing,
            usize page_index) noexcept;
        void (*lease_release)(void* backing, Page page) noexcept;
        libk::Expected<PageSlotState, MemoryError> (*page_state)(
            const void* backing,
            usize page_index) noexcept;
        libk::Expected<void, MemoryError> (*observe_usage)(
            void* backing,
            usize page_index,
            bool accessed,
            bool dirty) noexcept;
        usize (*drain_page_waiters)(
            void* backing,
            usize capacity) noexcept;
        /*luna change: expose one bounded backing service hook, reason: MemoryExecutor invokes canonical PageRequest/PageSlot transitions through MemoryObject*/
        MemoryServiceBatch (*service)(
            void* backing,
            usize capacity) noexcept;
        /*luna change: expose one bounded candidate pass beside transport
          service, reason: PageReclaimer selects backing policy while PageSlot
          remains the sole page-state owner*/
        ReclaimResult (*reclaim)(
            void* backing,
            usize capacity) noexcept;
        libk::Expected<WritebackKey, MemoryError> (*queue_writeback)(
            void* backing,
            usize page_index) noexcept;
        libk::Expected<void, MemoryError> (*writeback)(
            void* backing,
            pager::Pager* pager,
            usize page_index,
            WritebackTxn txn) noexcept;
        libk::Expected<void, MemoryError> (*evict_page)(
            void* backing,
            usize page_index) noexcept;
        void (*destroy)(void* backing) noexcept;
        void (*stop)(void* backing) noexcept;
    };

    [[nodiscard]] auto initialize_backing(
        BackingKind kind,
        libk::Span<const MemoryExtent> extents,
        AnonymousConfig anonymous,
        BootOwnership boot_ownership,
        OwnedPageGroup&& boot_pages,
        kernel::pager::Pager* pager,
        AccessMask pager_access,
        object::ObjectRef&& pager_ref) noexcept
        -> libk::Expected<void, MemoryError>;
    [[nodiscard]] auto materialize_impl(
        usize page_index,
        FrameDemand* demand,
        WaitRelation* relation,
        void* owner,
        WaitRelation::Publish publish) noexcept
        -> libk::Expected<PageLease, MemoryError>;
    /*luna change: settle the single fault pin only through its owner, reason: PageFault cancellation and terminal release must not become public lifetime APIs*/
    void release_fault() noexcept;
    [[nodiscard]] auto release_pressure(
        WaitRelation& relation,
        u64 generation) noexcept -> bool;
    [[nodiscard]] auto cancel_fault(
        WaitRelation& relation,
        u64 generation) noexcept -> bool;
    [[nodiscard]] auto detach(MemoryAttachment& attachment) noexcept -> bool;
    void drop_page() noexcept;
    void finish_transfer(
        usize page_index,
        OwnedPage&& page,
        bool commit) noexcept;
    void finish_retire() noexcept;
    void fail_build() noexcept;
    void bind_sponsor(kernel::resource::Sponsorship& sponsor) noexcept;
    [[nodiscard]] auto reserve_dynamic(kernel::resource::Budget charge) noexcept
        -> libk::Expected<kernel::resource::Reservation, MemoryError>;
    void release_lease(Page page) noexcept;
    void release_backing_hold() noexcept;
    void schedule_work() noexcept;
    void finish_work() noexcept;
    [[nodiscard]] auto try_reclaim_pin() noexcept -> bool;
    void finish_reclaim_pin() noexcept;
    [[nodiscard]] auto reclaim(usize capacity) noexcept -> ReclaimResult;
    [[nodiscard]] auto service(usize capacity) noexcept -> MemoryServiceBatch;
    void ensure_work_observation() noexcept;
    void publish_work_observation(const MemoryServiceBatch& batch) noexcept;

    Pmm* pmm_{};
    MemoryExecutor* work_{};
    PageReclaimer& reclaimer_;
    usize logical_pages_{};
    mutable kernel::sync::SpinLock<kernel::sync::LockClass::MemoryObject>
        lock_{};
    AttachmentList attachments_{};
    void* backing_{};
    const BackingOps* backing_ops_{};
    OwnedPage backing_page_{};
    kernel::resource::Sponsorship backing_sponsorship_{};
    usize operations_{};
    MemoryState state_{MemoryState::Building};
    SealState seal_{SealState::Loadable};
    ContentEpoch content_epoch_{};
    AccessMask access_{};
    object::ObjectRef pager_ref_{};
    bool releasing_{};
    /*luna change: retain one intrusive work node and one operations pin gate, reason: queue membership, admission, and lifetime remain separate canonical concerns*/
    libk::IntrusiveListHook work_hook_{};
    /*luna change: make admission and pin visibility atomic across owner/executor locks, reason: retire and submit cross lock domains without a data race*/
    libk::Atomic<bool> work_open_{true};
    libk::Atomic<bool> work_active_{false};
    libk::Atomic<bool> observation_reserved_{false};
    libk::Atomic<u64> observation_key_{};
    usize mapping_count_{};
    ReclaimEntry reclaim_entry_{};
    kernel::resource::Sponsorship* sponsor_{};
};

} // namespace kernel::mm
