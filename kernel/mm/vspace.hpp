#pragma once

#include <base/types.hpp>
#include <cap/cap.hpp>
#include <cap/grant.hpp>
#include <expected>
#include <libk/assert.hpp>
#include <libk/delegate.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/intrusive_tree.hpp>
#include <libk/manual_lifetime.hpp>
#include <libk/noncopyable.hpp>
#include <libk/sync/atomic.hpp>
#include <mm/kspace.hpp>
#include <mm/mem.hpp>
#include <mm/pmm.hpp>
#include <mm/table.hpp>
#include <mm/tlb.hpp>
#include <mm/types.hpp>
#include <object/ref.hpp>
#include <sync.hpp>
#include <utility>
#include <variant>

#include <resource/sponsorship.hpp>

class Env;

class CpuRegistry;
namespace object {
template <typename T> struct traits;
}
namespace cap {
class CSpace;
template <typename T> class Resolved;
} // namespace cap

namespace mm {
class VSpace;
// A committed translation fence retains the accepted identity through drain.
class Fence final : private libk::noncopyable_nonmovable {
  public:
    Fence(object::ref<>&& target, mm::VSpace& space) noexcept;
    ~Fence() noexcept;
    auto completion() noexcept -> Completion& { return receipt_.completion(); }
    void start() noexcept;

  private:
    friend class mm::VSpace;

    object::ref<> target_;
    mm::VSpace* space_{};
    libk::IntrusiveListHook hook_{};
    bool ready_{}; // VSpace lock owns selection; Completion owns publication.
    Receipt receipt_;
};

class Map;
class VSpace;

struct ViewReq final {
    object::ref<> memory{};
    ObjectRange object{};
    VRange virtual_range{};
    Perms perms{Perms::of(Perm::Read, Perm::Write)};
};

class Backing;
class Borrow final : private libk::noncopyable_nonmovable {
  public:
    Borrow(VSpace& owner, Backing& backing, ViewReq&& req) noexcept
        : owner_(&owner), backing_(&backing), range_(req.virtual_range), memory_(std::move(req.memory)),
          object_(req.object), perms_(req.perms) {}
    ~Borrow() noexcept;
    libk::IntrusiveListHook hook_{};

  private:
    friend class VSpace;
    friend class View;
    VSpace* owner_{};
    Backing* backing_{};
    VRange range_{};
    object::ref<> memory_{};
    ObjectRange object_{};
    Perms perms_{};
};

class View final {
  public:
    View() noexcept = default;
    View(View&&) noexcept = default;
    auto operator=(View&&) noexcept -> View& = default;
    explicit operator bool() const noexcept { return bool(h_); }
    bool valid() const noexcept;
    auto virtual_range() const noexcept -> VRange { return h_ ? h_.get()->range_ : VRange{}; }
    auto object_range() const noexcept -> ObjectRange { return h_.get()->object_; }
    auto perms() const noexcept -> Perms { return h_.get()->perms_; }
    void reset() noexcept { h_.reset(); }

  private:
    friend class VSpace;
    struct Drop {
        void operator()(Borrow* borrow) const noexcept;
    };
    explicit View(Borrow& borrow) noexcept : h_(&borrow) {}
    libk::unique_handle<Borrow*, Drop> h_{};
};

enum class MapKind : u8 { Map, Reserved, Guard };

class Map final : private libk::noncopyable_nonmovable {
  public:
    Map(VRange range, ObjectRange object, Perms perms, Perms ceiling, Backing& backing) noexcept
        : range_(range), object_(object), perms_(perms), ceiling_(ceiling), binding_(&backing) {}
    Map(VRange range, MapKind kind) noexcept : kind_(kind), range_(range) {}
    ~Map() noexcept;
    auto range() const noexcept -> VRange { return range_; }
    auto key() const noexcept -> MapId { return key_; }
    auto perms() const noexcept -> Perms { return perms_; }
    auto ceiling() const noexcept -> Perms { return ceiling_; }
    auto object_range() const noexcept -> ObjectRange { return object_; }
    libk::IntrusiveTreeHook layout_hook_{};

  private:
    friend class VSpace;
    friend class Backing;
    MapKind kind_{MapKind::Map};
    VRange range_{};
    Map* pending_next_{};
    MapId key_{};
    ObjectRange object_{};
    Perms perms_{};
    Perms ceiling_{};
    Backing* binding_{};
    libk::IntrusiveListHook backing_hook_{};
};

struct LayoutCmp {
    bool operator()(const Map& a, const Map& b) const noexcept { return a.range().base() < b.range().base(); }
    bool operator()(Virt a, const Map& b) const noexcept { return a < b.range().base(); }
    bool operator()(const Map& a, Virt b) const noexcept { return a.range().base() < b; }
};
using LayoutTree = libk::IntrusiveTree<Map, &Map::layout_hook_, LayoutCmp>;

class VSpace;

class MapPage final : private libk::noncopyable_nonmovable {
  public:
    MapPage(Virt address, usize object_page, PageHold&& source) noexcept
        : address_(address), object_page_(object_page),
          storage_(std::in_place_type<PageHold>, std::move(source)) {}

    MapPage(Virt address, usize object_page, OwnedPage&& page, resource::Charge&& charge) noexcept
        : address_(address), object_page_(object_page),
          storage_(std::in_place_type<Private>, std::move(charge), std::move(page)) {}

    ~MapPage() noexcept;

    [[nodiscard]] auto address() const noexcept -> Virt { return address_; }
    [[nodiscard]] auto page() const noexcept -> Page {
        if (const auto* p = std::get_if<PageHold>(&storage_)) return p->page();
        return std::get_if<Private>(&storage_)->page.page();
    }
    [[nodiscard]] auto object_page() const noexcept -> usize { return object_page_; }
    [[nodiscard]] auto perms() const noexcept -> Perms {
        if (const auto* p = std::get_if<PageHold>(&storage_)) return p->perms();
        return Perms::of(Perm::Read, Perm::Write);
    }
    [[nodiscard]] auto private_owned() const noexcept -> bool {
        return std::get_if<Private>(&storage_) != nullptr;
    }

  private:
    friend class VSpace;
    friend class Backing;

    // An installed PTE retains its actual PageHold until TLB drain.
    // A private COW page owns its frame and charge.
    struct Private {
        resource::Charge charge;
        OwnedPage page;
    };

    auto take_charge() noexcept -> resource::Charge {
        if (auto* p = std::get_if<Private>(&storage_)) return std::move(p->charge);
        return {};
    }

    Virt address_{};
    usize object_page_{};
    std::variant<PageHold, Private> storage_;
    libk::IntrusiveTreeHook tree_hook_{};
    MapPage* pending_next_{};
    Backing* binding_{};
};

struct MapPageCmp final {
    [[nodiscard]] constexpr auto operator()(const MapPage& lhs, const MapPage& rhs) const noexcept -> bool {
        return lhs.address() < rhs.address();
    }
    [[nodiscard]] constexpr auto operator()(Virt lhs, const MapPage& rhs) const noexcept -> bool {
        return lhs < rhs.address();
    }
    [[nodiscard]] constexpr auto operator()(const MapPage& lhs, Virt rhs) const noexcept -> bool {
        return lhs.address() < rhs;
    }
};

class Backing final : private libk::noncopyable_nonmovable {
    using MappingList = libk::IntrusiveList<Map, &Map::backing_hook_>;
    using PageTree = libk::IntrusiveTree<MapPage, &MapPage::tree_hook_, MapPageCmp>;

  public:
    Backing(VSpace& owner, object::ref<>&& memory, Mem& object, Perms perms, bool private_write) noexcept;
    ~Backing() noexcept;

    [[nodiscard]] auto memory() noexcept -> Mem& { return *memory_; }

  private:
    friend class VSpace;

    [[nodiscard]] auto attach_memory() noexcept -> std::expected<void, MemErr>;
    [[nodiscard]] auto detach() noexcept -> bool;
    [[nodiscard]] auto drained() const noexcept -> bool;

    static void invalidate_memory(void* ctx, MemWork&& work) noexcept;
    static void released(void* ctx) noexcept;
    static void invalidate_grant(void* ctx, cap::GrantWork&& work, cap::GrantInvalidation reason) noexcept;

    static const MemOps memory_ops_;
    static const cap::GrantAttachmentOps grant_ops_;

    VSpace* owner_{};
    object::ref<> memory_ref_{};
    Mem* memory_{};
    Perms perms_{};
    bool private_write_{};
    MappingList mappings_{};
    PageTree pages_{};
    libk::IntrusiveList<Borrow, &Borrow::hook_> views_{};
    libk::IntrusiveListHook invalidation_hook_{};
    MemLink memory_attachment_;
    libk::ManualLifetime<cap::GrantAttachment> grant_attachment_{};
    libk::ManualLifetime<MemWork> memory_work_{};
    libk::ManualLifetime<cap::GrantWork> grant_work_{};
    bool invalid() const noexcept { return bool(memory_work_) || bool(grant_work_); }
    libk::IntrusiveListHook retired_hook_{};
};

class SpaceWork;

enum class VSpaceState : u8 {
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
    NotRam,
    GrantUnavailable,
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

using VSpaceServiceResult = std::expected<VSpaceServiceState, VSpaceServiceError>;

struct VmCtx final {
    CpuRegistry* cpus{};
    CpuId local{};
};

struct MapReq final {
    VRange virtual_range{};
    ObjectRange object{};
    Perms perms{};
    bool private_write{};
};

struct MapResult final {
    MapId mapping{};
    VmStatus status{VmStatus::Complete};
};

enum class FaultKind : u8 {
    NoMapping,
    Guard,
    AccessDenied,
    Busy,
    Pending,
    ResourceExhausted,
    OutOfMemory,
    BackingFailed,
    Ready,
    Materialized,
};

/* Keep VSpace error classification at the fault boundary. */
[[nodiscard]] auto fault_kind(VSpaceError error) noexcept -> FaultKind;
[[nodiscard]] auto fault_kind(MemErr error) noexcept -> FaultKind;

struct FaultResult final {
    FaultKind kind{FaultKind::NoMapping};
    MapId mapping{};
    usize object_page{};
    VmStatus status{VmStatus::Complete};
    Mem* memory{};
};

struct MapInfo final {
    MapId key{};
    VRange range{};
    ObjectRange object{};
    Perms perms{};
    Perms ceiling{};
};

class VSpace final : private libk::noncopyable_nonmovable {
    using InvalidationList = libk::IntrusiveList<Backing, &Backing::invalidation_hook_>;

  public:
    class Data {
        friend class VSpace;
        Data(Pmm& pmm, KSpace& kernel, SpaceWork& work, object::ref<>&& payer, resource::Charge&& charge,
             PageTable&& root) noexcept
            : pmm_(&pmm), kernel_(&kernel), work_(&work), payer_(std::move(payer)),
              charge_(std::move(charge)), root_(std::move(root)) {}
        Pmm* pmm_;
        KSpace* kernel_;
        SpaceWork* work_;
        object::ref<> payer_;
        resource::Charge charge_;
        PageTable root_;

      public:
        Data(Data&&) noexcept = default;
        Data(const Data&) = delete;
    };
    static auto prepare(const object::ref<>& payer, Pmm&, KSpace&, SpaceWork&) noexcept
        -> std::expected<Data, VSpaceError>;
    explicit VSpace(Data&&) noexcept;
    ~VSpace() noexcept;

    [[nodiscard]] auto state() const noexcept -> VSpaceState;
    [[nodiscard]] auto can_destroy_object(cap::VmLimit auth) const noexcept -> bool;
    [[nodiscard]] auto root() noexcept -> Root;
    [[nodiscard]] auto active_cpus() const noexcept -> CpuSet { return tlb_.active_cpus(); }
    [[nodiscard]] auto binding_count() const noexcept -> usize;

    // Binds a trusted kernel consumer to the unique live Map covering the
    // requested virtual/object ranges. MapId remains VSpace-internal;
    // callers cannot manufacture a second mapping identity truth.
    [[nodiscard]] auto bind_view(ViewReq&& request) noexcept -> std::expected<View, VSpaceError>;

    [[nodiscard]] auto reserve(VRange range, bool guard = false) noexcept -> std::expected<void, VSpaceError>;
    [[nodiscard]] auto clear(VmCtx, VRange) noexcept -> std::expected<VmStatus, VSpaceError>;

    [[nodiscard]] auto map(VmCtx ctx, MapReq request, object::ref<>&& memory, Mem& object,
                           cap::MemLimit auth) noexcept -> std::expected<MapResult, VSpaceError>;

    [[nodiscard]] auto map(VmCtx ctx, cap::VmLimit where, MapReq request, cap::Resolved<Mem>& memory) noexcept
        -> std::expected<MapResult, VSpaceError>;

    [[nodiscard]] auto unmap(VmCtx ctx, cap::VmLimit where, VRange range) noexcept
        -> std::expected<VmStatus, VSpaceError>;
    [[nodiscard]] auto unmap(VmCtx ctx, VRange range) noexcept -> std::expected<VmStatus, VSpaceError>;
    [[nodiscard]] auto protect(VmCtx ctx, cap::VmLimit where, VRange range, Perms perms) noexcept
        -> std::expected<VmStatus, VSpaceError>;
    [[nodiscard]] auto protect(VmCtx ctx, VRange range, Perms perms) noexcept
        -> std::expected<VmStatus, VSpaceError>;
    [[nodiscard]] auto fault(Thread&, VmCtx, Virt, Perm) noexcept -> FaultKind;
    [[nodiscard]] auto fault(VmCtx ctx, Virt address, Perm perms, WaitRelation* relation = nullptr,
                             void* owner = nullptr, WaitRelation::Publish publish = nullptr) noexcept
        -> std::expected<FaultResult, VSpaceError>;
    // Samples the architecture projection of one materialized PTE and folds
    // it into the Mem page truth. When clear is true the selected
    // A/D bits are cleared through the normal translation/shootdown path.
    [[nodiscard]] auto sample_usage(VmCtx ctx, Virt address, bool clear = false) noexcept
        -> std::expected<PageUsage, VSpaceError>;
    [[nodiscard]] auto inspect(MapId key) const noexcept -> std::expected<MapInfo, VSpaceError>;

    // Completes remote shootdowns and queued Memory/Grant invalidations.
    // Background continuation has a distinct retry/wait/fatal contract and
    // never leaks syscall-facing VSpaceError values to its executor.
    [[nodiscard]] auto service(VmCtx ctx) noexcept -> VSpaceServiceResult;
    [[nodiscard]] auto pending() const noexcept -> bool;
    void wait_pending(mm::Fence& wait) noexcept;

    void retire(object::cleanup&& cleanup) noexcept;

  private:
    friend class SpaceWork;
    friend class Backing;
    friend class ::Env;
    friend class View;
    friend struct object::traits<VSpace>;

    // Transaction-local table capacity.  charge is declared before pages so
    // physical ownership is released first during reverse destruction.
    struct TableReserve final {
        TableReserve(resource::Charge&& table_charge, PageGroup&& table_pages) noexcept
            : charge(std::move(table_charge)), pages(std::move(table_pages)) {}

        TableReserve(TableReserve&&) noexcept = default;
        auto operator=(TableReserve&&) noexcept -> TableReserve& = default;

        resource::Charge charge{};
        PageGroup pages{};
    };

    [[nodiscard]] auto map_impl(VmCtx ctx, MapReq request, object::ref<>&& memory_ref, Mem& memory,
                                cap::MemLimit mem_auth, Perms vspace_access,
                                cap::Resolved<Mem>* capability) noexcept
        -> std::expected<MapResult, VSpaceError>;
    [[nodiscard]] auto edit(VmCtx, VRange, std::optional<Perms>, bool clear = false) noexcept
        -> std::expected<VmStatus, VSpaceError>;
    [[nodiscard]] static auto valid_user_range(VRange range) noexcept -> bool;
    [[nodiscard]] auto overlap(VRange range) noexcept -> Map*;
    [[nodiscard]] auto find(VRange range) noexcept -> Map*;

    [[nodiscard]] auto begin_edit(VRange range, bool must_be_empty) noexcept
        -> std::expected<void, VSpaceError>;
    void end_edit() noexcept;
    struct EditDrop {
        void operator()(VSpace* space) const noexcept { space->end_edit(); }
    };
    using Edit = libk::unique_handle<VSpace*, EditDrop>;

    [[nodiscard]] auto commit_flush(Tlb::Edit&& mutation, VmCtx ctx, Flush& retire, resource::Charge& refund,
                                    bool instruction_sync = false) noexcept -> VmStatus;
    [[nodiscard]] auto finish_pending(resource::Charge& refund) noexcept -> bool;
    void queue_layout(Map& node) noexcept;
    void queue_page(MapPage& page) noexcept;
    void queue_binding(Backing& auth) noexcept;
    void destroy_layout(Map& node) noexcept;
    void detach_mapping(Map& mapping) noexcept;
    void release_page(MapPage& page, resource::Charge& refund) noexcept;
    void finish_bindings() noexcept;
    void finish_waiters() noexcept;
    libk::IntrusiveList<mm::Fence, &mm::Fence::hook_> waiters_{};

    void request_invalidation(Backing& auth, MemWork&& work) noexcept;
    void request_invalidation(Backing& auth, cap::GrantWork&& work) noexcept;
    [[nodiscard]] auto start_invalidation(VmCtx ctx, Backing& auth) noexcept
        -> std::expected<VmStatus, VSpaceError>;

    [[nodiscard]] auto make_fragment(Map& source, VRange range, Perms perms) noexcept
        -> std::expected<Map*, VSpaceError>;

    [[nodiscard]] auto materialize_fault(VmCtx ctx, Map& mapping, Virt page_address, usize object_page,
                                         WaitRelation* relation, void* owner,
                                         WaitRelation::Publish publish) noexcept
        -> std::expected<FaultResult, VSpaceError>;
    [[nodiscard]] auto copy_private_fault(VmCtx ctx, Map& mapping, MapPage& source) noexcept
        -> std::expected<FaultResult, VSpaceError>;
    [[nodiscard]] auto fold_usage(MapPage& page, PageUsage observed) noexcept
        -> std::expected<PageUsage, VSpaceError>;
    [[nodiscard]] auto reserve_tables(MapPage* pages) noexcept -> std::expected<TableReserve, VSpaceError>;
    void commit_tables(TableReserve& reserve) noexcept;
    void retire_table(Flush& retire, OwnedPage&& page) noexcept;
    void release_root() noexcept;

    void try_finish_retire() noexcept;
    void complete_cleanup() noexcept;
    void flush_ready() noexcept;
    void schedule_work() noexcept;
    [[nodiscard]] auto work_ready() const noexcept -> bool;
    [[nodiscard]] auto prepare_retire() noexcept -> bool;
    [[nodiscard]] auto attach_execution() noexcept -> bool;
    void detach_execution() noexcept;
    void detach_view(Borrow& relation) noexcept;
    [[nodiscard]] auto view_active(const Borrow& relation) const noexcept -> bool;
    void invalidate_views(Map& mapping) noexcept;
    bool borrowed(const Map&, VRange) const noexcept;

    struct Receipt {
        Receipt(Pmm& pmm, VSpace& space) noexcept
            : flush(pmm, sync::Latch::Notifier::bind<&VSpace::flush_ready>(space)) {}
        Flush flush;
        Map* maps{};
        MapPage* pages{};
    };

    Pmm* pmm_{};
    KSpace* kernel_{};
    SpaceWork* work_{};
    mutable sync::Spin lock_{};
    libk::ManualLifetime<mm::PageTable> root_{};
    Tlb tlb_{};
    object::ref<> payer_{};
    Slab<Map> mappings_;
    Slab<Backing> binding_pool_;
    Slab<MapPage> pages_;
    Slab<Borrow> views_;
    // One preparer owns layout/backing pointers until its local resources drain.
    bool editing_{};
    LayoutTree layout_{};
    InvalidationList invalidations_{};
    libk::IntrusiveList<Backing, &Backing::retired_hook_> retired_{};
    bool draining_{}; // One stack owns external relation detach/refund callbacks.
    libk::ManualLifetime<Receipt> receipt_{};
    libk::ManualLifetime<object::cleanup> cleanup_{};
    libk::IntrusiveListHook work_hook_{};
    libk::Atomic<bool> work_open_{false};
    VSpaceState state_{VSpaceState::Live};
    usize bindings_{};
    usize ipi_retries_{};
    resource::Charge table_charge_{};
};

struct SpaceBatch final {
    usize processed{};
    usize progressed{};
    bool more{};
};

// Bounded executor index for VSpace continuations. VSpace pending state is the
// work truth; this queue only says that the state is currently actionable.
class SpaceWork final : private libk::noncopyable_nonmovable {
    using Queue = libk::IntrusiveList<VSpace, &VSpace::work_hook_>;

  public:
    using Notifier = libk::delegate<void() noexcept>;

    SpaceWork() noexcept = default;
    ~SpaceWork() noexcept;

    void submit(VSpace& space) noexcept;
    [[nodiscard]] auto run(VmCtx ctx, usize budget) noexcept -> SpaceBatch;
    [[nodiscard]] auto pending() const noexcept -> bool;

    void bind_notifier(Notifier notifier) noexcept;
    void unbind_notifier() noexcept;

  private:
    friend class VSpace;

    [[nodiscard]] auto take() noexcept -> VSpace*;
    void withdraw(VSpace& space) noexcept;

    mutable sync::Spin lock_{};
    Queue queue_{};
    Notifier notifier_{};
};

} // namespace mm
