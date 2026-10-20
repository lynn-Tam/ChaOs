#pragma once
#include <uapi/mem.h>
#include <work.hpp>
#include <op.hpp>

#include <libk/types.hpp>
#include <cap/cap.hpp>
#include <cap/grant.hpp>
#include <expected>
#include <type_traits>
#include <libk/assert.hpp>
#include <libk/delegate.hpp>
#include <libk/intrusive_list.hpp>
#include <libk/intrusive_tree.hpp>
#include <libk/manual_lifetime.hpp>
#include <libk/noncopyable.hpp>
#include <atomic>
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

#include <resource/account.hpp>

class Env;

class Cpus;
namespace object {
template <typename T> struct traits;
}
namespace cap {
class CSpace;
template <typename T> class Resolved;
} // namespace cap

namespace mm {
class VSpace;
class VmOp;
class Map;
class VSpace;

struct ViewReq final {
    static constexpr usize max_pages = IPC_BUFFER_MAX_PAGES;
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
    libk::intrusive_list_hook hook_{};

  private:
    friend class VSpace;
    friend class View;
    using Pages = libk::inplace_vector<PageHold, ViewReq::max_pages>;
    static constexpr usize closed = usize{1} << 63;
    Pages pages_;
    std::atomic<usize> uses_{closed};
    auto close() noexcept -> bool { return uses_.fetch_or(closed, std::memory_order_acq_rel) == 0; }
    auto idle() const noexcept -> bool { return !(uses_.load(std::memory_order_acquire) & ~closed); }
    void leave() noexcept;
    VSpace* owner_{};
    Backing* backing_{};
    VRange range_{};
    object::ref<> memory_{};
    ObjectRange object_{};
    Perms perms_{};
};

class View final {
  public:
    // A View outlives its accesses. Mapping closure drains these bounded
    // copies, then releases residency independently of the View's lifetime.
    class Access final {
        friend class View;
        struct Drop { void operator()(Borrow* borrow) const noexcept { borrow->leave(); } };
        libk::unique_handle<Borrow*, Drop> h_;
        explicit Access(Borrow& borrow) noexcept : h_(&borrow) {}
        auto copy(usize offset, std::span<byte>, bool write) const noexcept -> bool;
        auto region(usize offset, usize size, Perm) const noexcept -> std::span<byte>;
    public:
        Access(Access&&) noexcept = default;
        auto operator=(Access&&) noexcept -> Access& = default;
        template<Perm Mode = Perm::Read>
        auto bytes(usize offset, usize size) const noexcept -> std::span<std::conditional_t<Mode == Perm::Write, byte, const byte>> {
            static_assert(Mode == Perm::Read || Mode == Perm::Write);
            return region(offset, size, Mode);
        }
        auto read(usize offset, std::span<byte> output) const noexcept -> bool { return copy(offset, output, false); }
        auto write(usize offset, std::span<const byte> input) const noexcept -> bool {
            return copy(offset, {const_cast<byte*>(input.data()), input.size()}, true);
        }
    };
    View() noexcept = default;
    View(View&&) noexcept = default;
    auto operator=(View&&) noexcept -> View& = default;
    explicit operator bool() const noexcept { return bool(h_); }
    bool valid() const noexcept;
    auto size() const noexcept -> usize { return h_ ? h_.get()->object_.size() * page_size : 0; }
    auto access() const noexcept -> std::optional<Access>;
    auto read(usize offset, std::span<byte> output) const noexcept -> bool {
        auto permit = access(); return permit && permit->read(offset, output);
    }
    auto write(usize offset, std::span<const byte> input) const noexcept -> bool {
        auto permit = access(); return permit && permit->write(offset, input);
    }
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
    libk::intrusive_tree_hook layout_hook_{};

  private:
    friend class VSpace;
    friend class VmOp;
    friend class Backing;
    MapKind kind_{MapKind::Map};
    VRange range_{};
    Map* pending_next_{};
    MapId key_{};
    ObjectRange object_{};
    Perms perms_{};
    Perms ceiling_{};
    Backing* binding_{};
    libk::intrusive_list_hook backing_hook_{};
};

struct LayoutCmp {
    bool operator()(const Map& a, const Map& b) const noexcept { return a.range().base() < b.range().base(); }
    bool operator()(va_t a, const Map& b) const noexcept { return a < b.range().base(); }
    bool operator()(const Map& a, va_t b) const noexcept { return a.range().base() < b; }
};
using LayoutTree = libk::intrusive_tree<Map, &Map::layout_hook_, LayoutCmp>;

class VSpace;

class MapPage final : private libk::noncopyable_nonmovable {
  public:
    MapPage(va_t address, usize object_page, PageHold&& source) noexcept
        : address_(address), object_page_(object_page),
          storage_(std::in_place_type<PageHold>, std::move(source)) {}

    MapPage(va_t address, usize object_page, OwnedPage&& page, resource::Charge&& charge) noexcept
        : address_(address), object_page_(object_page),
          storage_(std::in_place_type<Private>, std::move(charge), std::move(page)) {}

    ~MapPage() noexcept;

    [[nodiscard]] auto address() const noexcept -> va_t { return address_; }
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
    friend class VmOp;
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

    va_t address_{};
    usize object_page_{};
    std::variant<PageHold, Private> storage_;
    libk::intrusive_tree_hook tree_hook_{};
    MapPage* pending_next_{};
    Backing* binding_{};
};

struct MapPageCmp final {
    [[nodiscard]] constexpr auto operator()(const MapPage& lhs, const MapPage& rhs) const noexcept -> bool {
        return lhs.address() < rhs.address();
    }
    [[nodiscard]] constexpr auto operator()(va_t lhs, const MapPage& rhs) const noexcept -> bool {
        return lhs < rhs.address();
    }
    [[nodiscard]] constexpr auto operator()(const MapPage& lhs, va_t rhs) const noexcept -> bool {
        return lhs.address() < rhs;
    }
};

class Backing final : private libk::noncopyable_nonmovable {
    using MappingList = libk::intrusive_list<Map, &Map::backing_hook_>;
    using PageTree = libk::intrusive_tree<MapPage, &MapPage::tree_hook_, MapPageCmp>;

  public:
    Backing(VSpace& owner, object::ref<> memory, Mem& object, Perms perms, bool private_write) noexcept;
    ~Backing() noexcept;

    [[nodiscard]] auto memory() noexcept -> Mem& { return *memory_; }

  private:
    friend class VSpace;
    friend class VmOp;

    [[nodiscard]] auto attach_memory() noexcept -> std::expected<void, MemErr>;
    [[nodiscard]] auto detach() noexcept -> bool;
    [[nodiscard]] auto drained() const noexcept -> bool;

    static void invalidate_memory(void* ctx, MemWork&& work) noexcept;
    static void released(void* ctx, MemWork&& terminal) noexcept;
    static void released(void* ctx, object::LinkWork&& done) noexcept;
    static void invalidate_grant(void* ctx, object::LinkWork&& work) noexcept;

    static const MemOps memory_ops_;
    static const cap::AttachOps grant_ops_;

    VSpace* owner_{};
    object::ref<> memory_ref_{};
    Mem* memory_{};
    Perms perms_{};
    bool private_write_{};
    MappingList mappings_{};
    PageTree pages_{};
    libk::intrusive_list<Borrow, &Borrow::hook_> views_{};
    libk::intrusive_list_hook invalidation_hook_{};
    MemLink memory_attachment_;
    libk::manual_lifetime<cap::Attachment> grant_attachment_{};
    libk::manual_lifetime<MemWork> memory_work_{};
    libk::manual_lifetime<object::LinkWork> grant_work_{};
    va_t trim_cursor_{};
    bool trim_scanned_{};
    bool invalid() const noexcept { return bool(memory_work_) || bool(grant_work_); }
    libk::intrusive_list_hook retired_hook_{};
};


enum class VSpaceState : u8 {
    Live = VSPACE_LIVE,
    Stopping = VSPACE_STOPPING,
    Quiescent = VSPACE_QUIESCENT,
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
    Canceled,
};
auto status(VSpaceError) noexcept -> status_t;

enum class VmStatus : u8 {
    Complete,
    Pending,
};

struct VmCtx final {
    Cpus* cpus{};
    CpuId local{};
    Op::Submission submission{};
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
};

struct MapInfo final {
    MapId key{};
    VRange range{};
    ObjectRange object{};
    Perms perms{};
    Perms ceiling{};
};

// One transaction owns its hardware receipt, retired records and refunds.
// Public operations and VSpace's prepaid control record use this same engine.
class VmOp final : private libk::noncopyable_nonmovable {
    friend class VSpace;
    using Retired = libk::intrusive_list<Backing, &Backing::retired_hook_>;
public:
    static constexpr bool fixed_bill = true;
    VmOp(Pmm&, VSpace&) noexcept;
    explicit VmOp(object::ref<>) noexcept;
    ~VmOp() noexcept;
    auto op() noexcept -> Op& { return state_; }
    void bind_work(WorkQueue& work, object::Key owner) noexcept { state_.bind(work, owner); }
    void retire(object::cleanup&& cleanup) noexcept { state_.retire(std::move(cleanup)); }
private:
    auto drain_pages() noexcept -> bool; // VSpace lock held.
    auto finish() noexcept -> bool; // No provider/Grant/Gate lock held.
    void complete() noexcept { state_.finish(error_ ? status(*error_) : STATUS_OK); }
    auto ready() const noexcept -> bool { return state_.ready(); }
    VSpace* owner_;
    Op state_;
    Flush flush_;
    Map* maps_{};
    MapPage* pages_{};
    Retired retired_;
    resource::Charge refund_;
    bool released_{}, flushed_{};
    std::optional<VSpaceError> error_;
    struct Edit {
        enum Phase : u8 { Scan, Views, Check, Prepare, Apply, Revoke, Pages, Done } phase{Scan};
        VRange range;
        std::optional<Perms> perms;
        bool clear{}, batch{}, stale{};
        Map *cursor{}, *first{}, *last{};
        Borrow* view{};
        va_t covered{}, page{};
        std::array<Map*, 2> tails{};
    };
    std::optional<Edit> edit_;
};

class VSpace final : private libk::noncopyable_nonmovable {
    using InvalidationList = libk::intrusive_list<Backing, &Backing::invalidation_hook_>;

  public:
    class Data {
        friend class VSpace;
        Data(Pmm& pmm, KSpace& kernel, WorkQueue& work, object::ref<> payer, resource::Charge&& charge,
             PageTable&& root) noexcept
            : pmm_(&pmm), kernel_(&kernel), work_(&work), payer_(std::move(payer)),
              charge_(std::move(charge)), root_(std::move(root)) {}
        Pmm* pmm_;
        KSpace* kernel_;
        WorkQueue* work_;
        object::ref<> payer_;
        resource::Charge charge_;
        PageTable root_;

      public:
        Data(Data&&) noexcept = default;
        Data(const Data&) = delete;
    };
    static auto prepare(const object::ref<>& payer, Pmm&, KSpace&, WorkQueue&) noexcept
        -> std::expected<Data, VSpaceError>;
    explicit VSpace(Data&&) noexcept;
    ~VSpace() noexcept;
    void bind_work(WorkQueue& work, object::Key owner) noexcept { job_.bind(owner); settled_.bind_work(work, owner); }

    template<class Transaction> auto billing(const object::ref<>& to, Transaction&& transaction) noexcept
        -> std::expected<u64, status_t> {
        {
            sync::Lock guard{lock_};
            if (state_ != VSpaceState::Live || editing_ || draining_ || active_ || !invalidations_.empty()) return std::unexpected(STATUS_BUSY);
            editing_ = true;
        }
        libk::scope_exit resume{[&]() noexcept {
            { sync::Lock guard{lock_}; editing_ = false; }
            work_->post(job_);
        }};
        object::ref<> payer{to};
        return transaction([&](auto&& visit) {
            visit(table_charge_);
            pages_.each([&](MapPage& page) {
                if (auto* owned = std::get_if<MapPage::Private>(&page.storage_)) visit(owned->charge);
            });
            mappings_.charges(visit); binding_pool_.charges(visit); pages_.charges(visit); views_.charges(visit);
        }, [&] { std::swap(payer_, payer); });
    }
    [[nodiscard]] auto state() const noexcept -> VSpaceState;
    auto info(u64) const noexcept -> VSpaceInfo;
    auto links(u64 cursor, u64 epoch, LinkRecords&) const noexcept -> status_t;
    auto inspect(VRange scope, va_t cursor) const noexcept -> std::expected<VmRecords, VSpaceError>;
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

    [[nodiscard]] auto map(VmCtx ctx, MapReq request, object::ref<> memory, Mem& object,
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
    [[nodiscard]] auto fault(Thread&, VmCtx, va_t, Perm) noexcept -> FaultKind;
    [[nodiscard]] auto fault(VmCtx ctx, va_t address, Perm perms, Source::Context* context = nullptr) noexcept
        -> std::expected<FaultResult, VSpaceError>;
    // Samples the architecture projection of one materialized PTE and folds
    // it into the Mem page truth. When clear is true the selected
    // A/D bits are cleared through the normal translation/shootdown path.
    [[nodiscard]] auto sample_usage(VmCtx ctx, va_t address, bool clear = false) noexcept
        -> std::expected<PageUsage, VSpaceError>;
    [[nodiscard]] auto inspect(MapId key) const noexcept -> std::expected<MapInfo, VSpaceError>;

    [[nodiscard]] auto pending() const noexcept -> bool;
    auto wait_pending(Thread&, Cpus&, sched::Dispatcher&) noexcept -> WaitResult;

    void retire(object::cleanup&& cleanup) noexcept;

  private:
    friend class Backing;
    friend class VmOp;
    friend class ::Env;
    friend class View;
    friend class Borrow;
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

    [[nodiscard]] auto map_impl(VmCtx ctx, MapReq request, object::ref<> memory_ref, Mem& memory,
                                cap::MemLimit mem_auth, Perms vspace_access,
                                cap::Resolved<Mem>* capability) noexcept
        -> std::expected<MapResult, VSpaceError>;
    [[nodiscard]] auto edit(VmCtx, VRange, std::optional<Perms>, bool clear = false) noexcept
        -> std::expected<VmStatus, VSpaceError>;
    auto advance_edit(VmCtx) noexcept -> std::expected<VmStatus, VSpaceError>;
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

    [[nodiscard]] auto commit_flush(Tlb::Edit&& mutation, VmCtx ctx, Flush& retire,
                                    bool instruction_sync = false) noexcept -> VmStatus;
    void queue_layout(Map& node) noexcept;
    void queue_page(MapPage& page, bool removed = true) noexcept;
    void queue_binding(Backing& auth) noexcept;
    void destroy_layout(Map& node) noexcept;
    void detach_mapping(Map& mapping) noexcept;
    void release_page(MapPage& page, resource::Charge& refund) noexcept;
    void finish_bindings() noexcept;
    auto pending_locked() const noexcept -> bool;
    void settle() noexcept;
    Src<SOURCE_READY> settled_{Source::Drained::bind<&VSpace::complete_cleanup>(*this)};

    void request_invalidation(Backing& auth, MemWork&& work) noexcept;
    void request_invalidation(Backing& auth, object::LinkWork&& work) noexcept;
    [[nodiscard]] auto start_invalidation(VmCtx ctx, Backing& auth) noexcept
        -> std::expected<VmStatus, VSpaceError>;

    [[nodiscard]] auto make_fragment(Map& source, VRange range, Perms perms) noexcept
        -> std::expected<Map*, VSpaceError>;

    [[nodiscard]] auto materialize_fault(VmCtx ctx, Map& mapping, va_t page_address, usize object_page,
                                         Source::Context* context) noexcept
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
    void run_work() noexcept;
    void advance() noexcept;
    [[nodiscard]] auto step(VmCtx ctx) noexcept -> bool;
    void flush_ready() noexcept;
    [[nodiscard]] auto work_ready() const noexcept -> bool;
    [[nodiscard]] auto attach_execution() noexcept -> bool;
    void detach_execution() noexcept;
    void detach_view(Borrow& relation) noexcept;

    Pmm* pmm_{};
    KSpace* kernel_{};
    WorkQueue* work_{};
    mutable sync::Spin lock_{};
    libk::manual_lifetime<mm::PageTable> root_{};
    Tlb tlb_{};
    object::ref<> payer_{};
    Slab<Map> mappings_;
    Slab<Backing> binding_pool_;
    Slab<MapPage> pages_;
    Slab<Borrow> views_;
    // One preparer owns layout/backing pointers until its local resources drain.
    bool editing_{};
    bool advancing_{}, again_{};
    LayoutTree layout_{};
    u64 mapped_{}, resident_{}, layout_epoch_{1}, observed_epoch_{};
    void link(Map&) noexcept;
    void unlink(Map&) noexcept;
    InvalidationList invalidations_{};
    bool draining_{}; // One stack owns external relation detach/refund callbacks.
    libk::manual_lifetime<VmOp> control_;
    VmOp* active_{};
    Borrow* waiting_view_{};
    object::ref<> operation_;
    auto transaction() noexcept -> VmOp&;
    auto accept(VmCtx, bool commit = true) noexcept -> std::expected<void, VSpaceError>;
    libk::manual_lifetime<object::cleanup> cleanup_{};
    Work job_{};
    VSpaceState state_{VSpaceState::Live};
    usize bindings_{};
    usize ipi_retries_{};
    resource::Charge table_charge_{};
};

} // namespace mm
