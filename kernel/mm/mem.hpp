#pragma once
#include <mm/slab.hpp>
#include <uapi/mem.h>

#include <libk/unique_handle.hpp>

#include <libk/types.hpp>
#include <cpu/types.hpp>
#include <expected>
#include <libk/intrusive_list.hpp>
#include <libk/noncopyable.hpp>
#include <libk/scope_guard.hpp>
#include <span>
#include <atomic>
#include <limits>
#include <mm/pmm.hpp>
#include <mm/phys.hpp>
#include <optional>
#include <sync.hpp>
#include <utility>
#include <variant>
#include <libk/intrusive_tree.hpp>
#include <wait.hpp>
#include <work.hpp>
#include <op.hpp>

class Thread;
class Cpus;
#include <mm/pager.hpp>

#include <mm/types.hpp>
#include <object/ref.hpp>
#include <resource/quota.hpp>

namespace object {
template <typename T> struct traits;
}

namespace mm {

class Mem;
struct Extent final {
    ObjectRange object{};
    Pages physical{};
    Perms perms{};
};

struct AnonCfg final {
    Perms perms{Perms::of(Perm::Read, Perm::Write)};
    bool eager{};
};

enum class BackingKind : u8 {
    Anonymous,
    Physical,
    Pager,
    Private,
};

enum class ContentState : u8 {
    Zero,
    Resident,
    Busy,
    Failed,
};

struct Frame final {
    struct Pins { std::atomic<usize> uses{}, wired{}; };
    Page page{};
    Perms perms{};
    Pins* pins{};
};

class Mem;
class MemWork;

enum class MemErr : u8 {
    InvalidSize,
    InvalidRange,
    InvalidAccess,
    NotRam,
    InvalidState,
    OutOfMemory,
    ResourceExhausted,
    GenerationExhausted,
    Busy,
    Dirty,
    Pending,
    Forwarded,
    BackingFailed,
    NotBacked,
    AttachmentState,
    OwnershipMismatch,
};

enum class MemState : u8 {
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

    [[nodiscard]] friend constexpr auto operator==(ContentEpoch, ContentEpoch) noexcept -> bool = default;
};

class MemLink;

// A source page remains owned by its staging Mem until the target
// pager backing accepts it.  Destruction aborts the transfer and restores the
// source slot, so a failed supply cannot leave two owners or no owner.
class PageTransfer final : private libk::noncopyable {
  public:
    PageTransfer() noexcept = default;
    PageTransfer(PageTransfer&& other) noexcept;
    auto operator=(PageTransfer&& other) noexcept -> PageTransfer&;
    ~PageTransfer() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept { return owner_ != nullptr; }
    [[nodiscard]] auto page() const noexcept -> Page { return page_.page(); }
    [[nodiscard]] auto take_page() noexcept -> OwnedPage { return std::move(page_); }
    void commit() noexcept;
    void abort() noexcept;

  private:
    friend class Mem;
    PageTransfer(Mem& owner, usize index, OwnedPage&& page) noexcept
        : owner_(&owner), index_(index), page_(std::move(page)) {}

    Mem* owner_{};
    usize index_{};
    OwnedPage page_{};
};

class MemWork final {
  public:
    MemWork() noexcept = default;
    MemWork(MemWork&&) noexcept = default;
    auto operator=(MemWork&&) noexcept -> MemWork& = default;
    explicit operator bool() const noexcept { return bool(h_); }
    void reset() noexcept { h_.reset(); }
    auto range() const noexcept -> ObjectRange { return h_ ? h_.get().range : ObjectRange{}; }

  private:
    friend class Mem;
    friend class MemLink;
    struct Data {
        MemLink* attachment{};
        Mem* lease{};
        ObjectRange range{};
        bool final_release{};
        static auto empty() noexcept -> Data { return {}; }
        static bool is_empty(const Data& d) noexcept { return !d.attachment; }
    };
    struct Drop {
        void operator()(Data&) const noexcept;
    };
    explicit MemWork(MemLink& a, Mem* lease = nullptr, ObjectRange r = {}, bool final_release = false) noexcept
        : h_(Data{&a, lease, r, final_release}) {}
    libk::unique_handle<Data, Drop, Data> h_{};
};

struct MemOps final {
    void (*invalidate)(void* context, MemWork&& work) noexcept;
    void (*released)(void* context, MemWork&& done) noexcept;
    bool wired{};
    u64 (*sample)(void*, std::span<PagerPage>, u64) noexcept {};
};

// Embedded in a Map and indexed non-owningly by Mem. The Map
// must retain its structural Mem reference until detach() completes.
class MemLink final : private libk::noncopyable_nonmovable {
  public:
    MemLink(void* context, const MemOps& ops) noexcept : context_(context), ops_(&ops) {}
    ~MemLink() noexcept;

    auto id() const noexcept -> u64 { return id_.value; }
    [[nodiscard]] auto attached() const noexcept -> bool;
    [[nodiscard]] auto busy() const noexcept -> bool;
    // Its consumer serializes permission changes through Mem::protect.
    auto perms() const noexcept -> Perms { return perms_; }
    // Returns true when no MemWork still pins the Map relation.
    [[nodiscard]] auto detach() noexcept -> bool;

  private:
    friend class Mem;
    friend class MemWork;

    enum class State : u8 {
        Idle,
        Attached,
        Invalidating,
        Detached,
        Releasing,
    };

    void drop_work(bool final_release) noexcept;

    libk::intrusive_tree_hook memory_hook_{};
    Mem* owner_{};
    object::identity<object::Link> id_{};
    void* context_{};
    const MemOps* ops_{};
    std::atomic<usize> work_{};
    sync::Spin lock_;
    std::atomic<u8> state_{static_cast<u8>(State::Idle)};
    Perms perms_{};
};

// Owns one Mem operation lease through the caller or installed PTE lifetime.
class PageHold final : private libk::noncopyable {
  public:
    PageHold() noexcept = default;
    PageHold(PageHold&&) noexcept = default;
    auto operator=(PageHold&&) noexcept -> PageHold& = default;
    ~PageHold() noexcept = default;

    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(h_); }
    [[nodiscard]] auto page() const noexcept -> Page { return h_.get().frame.page; }
    [[nodiscard]] auto perms() const noexcept -> Perms { return h_.get().frame.perms; }
    void reset() noexcept { h_.reset(); }

  private:
    friend class Mem;
    friend class Cache;
    PageHold(Mem& owner, Frame frame, bool wired) noexcept : h_(Data{&owner, frame, wired}) {
        if (frame.pins) {
            frame.pins->uses.fetch_add(1, std::memory_order_relaxed);
            if (wired) frame.pins->wired.fetch_add(1, std::memory_order_relaxed);
        }
    }

    struct Data {
        Mem* owner{};
        Frame frame{};
        bool wired{};
        static auto empty() noexcept -> Data { return {}; }
        static bool is_empty(const Data& d) noexcept { return !d.owner; }
    };
    struct Drop {
        void operator()(Data& d) const noexcept;
    };
    libk::unique_handle<Data, Drop, Data> h_{};
};

struct PagerData {
    Pager::Request request;
    Src<SOURCE_READY> completion;
    u64 dirty_epoch{}, usage_epoch{};
    bool failed{}, snapshot{}, accessed{};
    ~PagerData() noexcept { libk_assert(!request.pager); }
};

class Cache {
    friend class Mem;
    struct Node : PagerData {
        explicit Node(usize i) noexcept : index(i) {}
        usize index;
        resource::Usage usage;
        OwnedPage resident;
        bool buffered{};
        std::optional<MemErr> copy_error;
        u64 capacity_epoch{};
        PageHold borrowed;
        auto present() const noexcept -> bool { return bool(resident) || bool(borrowed); }
        auto page() const noexcept -> Page { return resident ? resident.page() : borrowed.page(); }
        Frame::Pins pins;
        libk::intrusive_tree_hook hook;
    };
    struct Compare {
        bool operator()(const Node& a, const Node& b) const noexcept { return a.index < b.index; }
        bool operator()(usize a, const Node& b) const noexcept { return a < b.index; }
        bool operator()(const Node& a, usize b) const noexcept { return a.index < b; }
    };
    struct Drop { Cache* cache; void operator()(Node*& n) noexcept { cache->rows_.destroy(*n); } };
    using Row = libk::unique_handle<Node*, Drop>;
    struct PagerCfg { Pager* pager; bool local; usize stop{}; };
  public:
    Cache(Pmm&, Perms, Pager* = nullptr, bool = false) noexcept;
    Cache(const Cache&) = delete;
    Cache(Cache&&) noexcept;
    ~Cache() noexcept;
    auto drain() noexcept -> bool;
    auto paged() const noexcept -> bool { return cfg_.pager != nullptr; }
    void bind(Mem&, WorkQueue&, object::Key) noexcept;
    auto snapshot(Mem&, usize, const PageHold&) noexcept -> std::expected<void, MemErr>;
    auto writable(usize) const noexcept -> bool;
    auto copy(usize, const object::ref<>&) noexcept -> std::expected<void, MemErr>;
    auto borrow(Mem&, usize, Source::Context*) noexcept -> std::expected<Frame, MemErr>;
    void records(PagerRecords&) noexcept;
    auto bind_pager(Pager&) noexcept -> std::expected<void, MemErr>;
    auto prepay(const object::ref<>&, usize) noexcept -> std::expected<bool, MemErr>;

    template<class Visit> void charges(Visit&& visit) noexcept {
        sync::Lock guard{tree_lock_};
        for (auto* node = nodes_.minimum(); node; node = nodes_.next(*node)) visit(node->usage);
        rows_.charges(visit);
    }
    auto query(usize) const noexcept -> ContentState;
    auto materialize(Mem&, usize, Source::Context*) noexcept
        -> std::expected<Frame, MemErr>;
    auto allocate(const object::ref<>&, usize) noexcept -> std::expected<Frame, MemErr>;
    auto begin_transfer(usize) noexcept -> std::expected<OwnedPage, MemErr>;
    auto restore_transfer(usize, OwnedPage&&) noexcept -> std::expected<void, MemErr>;
    auto commit_transfer(usize) noexcept -> std::expected<void, MemErr>;
    auto buffer(usize, bool refill) noexcept -> std::expected<void, MemErr>;
    auto stop(Mem&) noexcept -> bool;
    auto supply(Mem&, Pager&, u64, OwnedPage&&, cap::Graph::Admission, std::span<const object::ref<>*>, std::optional<Page> = {}, bool reclaim = true) noexcept -> std::expected<void, MemErr>;
    auto finish(Mem&, Pager&, u64, bool, cap::Graph::Admission, std::span<const object::ref<>*>, bool retry = false) noexcept -> std::expected<u32, MemErr>;
    auto complete(Pager::Reply&, bool, cap::Graph::Admission = {}, std::span<const object::ref<>*> = {}, bool retry = false) noexcept
        -> std::expected<u32, MemErr>;
    auto observe_usage(usize, bool, bool) noexcept -> std::expected<void, MemErr>;
    auto trim(ObjectRange, usize& cursor, bool& removing, usize& released) noexcept -> std::expected<bool, MemErr>;
  private:
    auto capacity(Mem&, usize, Source::Context*, MemErr, std::optional<u64> = {}) noexcept -> std::expected<Frame, MemErr>;
    auto make(const object::ref<>&, usize) noexcept -> std::expected<Row, MemErr>;
    static auto charge_page(const object::ref<>&) noexcept -> std::expected<resource::Usage, MemErr>;
    auto writeback_locked(Node&) noexcept -> bool;
    auto install(Mem&, Pager::Reply&, OwnedPage&&, cap::Graph::Admission,
        std::span<const object::ref<>*>, std::optional<Page> = {}, bool reclaim = true) noexcept -> std::expected<void, MemErr>;
    auto frame(Node& n) const noexcept -> Frame {
        auto p = perms_;
        if (n.borrowed || (cfg_.pager && !n.dirty_epoch)) p = p.intersect(Perms::of(Perm::Read, Perm::Execute));
        return {n.page(), p, &n.pins};
    }
    object::ref<> source_{};
    usize source_first_{};
    Mem* owner_{};
    Pmm* pmm_;
    Perms perms_;
    mutable sync::Spin tree_lock_;
    libk::intrusive_tree<Node, &Node::hook, Compare> nodes_;
    Slab<Node, false, false> rows_;
    PagerCfg cfg_{};
    u64 capacity_epoch_{};
    Node* capacity_{};
    Node* done_{};
    void completed(Mem&, usize, status_t, usize released) noexcept;
};

class Extents {
    struct Row { explicit Row(Extent e) noexcept : extent(e) {} Extent extent; Row* next{}; };
  public:
    explicit Extents(Pmm&) noexcept;
    Extents(const Extents&) = delete;
    Extents(Extents&&) noexcept;
    ~Extents() noexcept;
    auto drain() noexcept -> bool;
    auto initialize(const object::ref<>&, std::span<const Extent>, PageGroup&&, Phys::Claim&&) noexcept
        -> std::expected<void, MemErr>;
    template<class Visit> void charges(Visit&& visit) noexcept { rows_.charges(visit); }
    auto managed() const noexcept -> bool { return bool(owned_); }
    auto external() const noexcept -> Pages { return external_.range(); }
    auto query(usize) const noexcept -> ContentState;
    auto materialize(usize) const noexcept -> std::expected<Frame, MemErr>;
  private:
    auto find(usize) const noexcept -> const Extent*;
    void reset() noexcept;
    Slab<Row, false> rows_;
    Row* head_{};
    PageGroup owned_{};
    Phys::Claim external_;
};

struct PhysCfg { std::span<const Extent> extents; PageGroup owned; Phys::Claim external{}; };
struct CopyCfg { object::ref<> source; usize first{}; object::ref<> pager; };
struct PagedCfg { object::ref<> pager; Perms perms; bool local{}; usize credits{64}; };

class Mem final : private libk::noncopyable_nonmovable {
    using Store = std::variant<Cache, Extents>;
  public:
    static constexpr u64 trim_clean = 1, trim_dirty = 2, trim_busy = 4, trim_canceled = 8;
    using Config = std::variant<AnonCfg, PhysCfg, PagedCfg, CopyCfg>;
    class Data {
        friend class Mem;
        Data(Pmm& pmm, usize pages) noexcept : pmm_(&pmm), pages_(pages) {}
        object::ref<> quota_, pager_;
        Pager::Reservation reservation_;
        Pmm* pmm_;
        usize pages_, prepay_{};
        Perms perms_;
        std::optional<Store> store_;
      public:
        Data(Data&&) noexcept = default;
        Data(const Data&) = delete;
    };
    static auto prepare(const object::ref<>& quota, Pmm&, usize, Config) noexcept
        -> std::expected<Data, MemErr>;
    explicit Mem(Data&&) noexcept;
    ~Mem() noexcept;
    auto op() noexcept -> Op& { return initialization_; }
    void bind_work(WorkQueue&, object::Key) noexcept;

    template<class Transaction> auto reassign_usage(const object::ref<>& to, Transaction&& transaction) noexcept
        -> std::expected<u64, status_t> {
        {
            sync::Lock guard{lock_};
            if (state_ != MemState::Live || !work_open_.load(std::memory_order_acquire) || reassigning_ || preparing_ || trimming_ || releasing_
                || request_pins_.load(std::memory_order_acquire)) return std::unexpected(STATUS_BUSY);
            reassigning_ = true;
        }
        libk::scope_exit resume{[&]() noexcept {
            { sync::Lock guard{lock_}; reassigning_ = false; }
            finish_retire();
        }};
        object::ref<> quota{to};
        return transaction([&](auto&& visit) {
            std::visit([&](auto& store) { store.charges(visit); }, *store_);
        }, [&] { std::swap(quota_, quota); });
    }
    template<class Publish> auto admit(Publish&& publish, usize count) noexcept -> status_t { return backing_.admit(std::forward<Publish>(publish), {}, count); }
    void links(u64 cursor, LinkRecords& out) const noexcept;
    auto id() const noexcept -> u64 { return binding_.key; }
    auto bind_pager(object::ref<>, const cap::GrantPin* = nullptr, std::span<const object::ref<>*> = {}) noexcept -> std::expected<void, MemErr>;
    void records(PagerRecords&, bool sample = false) noexcept;
    auto writable(usize) const noexcept -> bool;
    auto private_pages() const noexcept -> bool;
    auto needs_copy(usize) const noexcept -> bool;
    auto make_private(usize, Source::Context&) noexcept -> std::expected<void, MemErr>;
    auto dirty(usize) noexcept -> std::expected<void, MemErr>;
    auto reclaim(usize, Source::Context&, PageHold&&, cap::Graph::Admission = {},
        std::span<const object::ref<>*> = {}, std::span<const cap::GrantPin*> = {}) noexcept -> std::expected<void, MemErr>;
    [[nodiscard]] auto size() const noexcept -> usize { return logical_pages_ * page_size; }
    [[nodiscard]] auto page_count() const noexcept -> usize { return logical_pages_; }
    [[nodiscard]] auto kind() const noexcept -> BackingKind;
    auto managed() const noexcept -> bool;
    [[nodiscard]] auto state() const noexcept -> MemState;
    [[nodiscard]] auto seal_state() const noexcept -> SealState;
    [[nodiscard]] auto content_epoch() const noexcept -> ContentEpoch;
    // Publishes immutable executable content once page preparation/transfers
    // and writable mapping attachments have drained.
    [[nodiscard]] auto seal() noexcept -> std::expected<void, MemErr>;
    [[nodiscard]] auto query(usize page_index) const noexcept -> std::expected<ContentState, MemErr>;
    [[nodiscard]] auto materialize(usize page_index, Source::Context* = nullptr, bool wired = false) noexcept
        -> std::expected<PageHold, MemErr>;
    auto wait_capacity(usize, Source::Context&, MemErr, u64 quota) noexcept -> std::expected<void, MemErr>;
    [[nodiscard]] auto begin_transfer(usize page_index) noexcept -> std::expected<PageTransfer, MemErr>;
    auto buffer(usize page, bool refill) noexcept -> std::expected<void, MemErr>;
    [[nodiscard]] auto supply(Pager& pager, u64 id, OwnedPage&& page, cap::Graph::Admission = {},
        std::span<const object::ref<>*> = {}) noexcept -> std::expected<void, MemErr>;
    [[nodiscard]] auto supply(Pager& pager, PageTransfer&& page, u64 id, cap::Graph::Admission = {},
        std::span<const object::ref<>*> = {}) noexcept
        -> std::expected<void, MemErr>;
    [[nodiscard]] auto supply(Pager&, const PageHold&, u64 id, cap::Graph::Admission = {},
        std::span<const object::ref<>*> = {}, bool reclaim = true) noexcept -> std::expected<void, MemErr>;
    [[nodiscard]] auto pager_finish(Pager& pager, u64 id, bool fail, cap::Graph::Admission = {},
        std::span<const object::ref<>*> = {}, bool retry = false) noexcept -> std::expected<u32, MemErr>;
    [[nodiscard]] auto observe_usage(usize page_index, bool accessed, bool dirty) noexcept
        -> std::expected<void, MemErr>;
    [[nodiscard]] auto trim(ObjectRange, Source::Context&, cap::Graph::Admission = {},
        std::span<const object::ref<>*> gates = {}, std::span<const cap::GrantPin*> grants = {}) noexcept -> std::expected<void, MemErr>;
    // Bounded anonymous RAM access pins storage, independent of CPU mappings;
    // seal and view permissions still exclude writes to immutable content.
    [[nodiscard]] auto write(usize offset, std::span<const byte> input) noexcept
        -> std::expected<void, MemErr>;
    [[nodiscard]] auto read(usize offset, std::span<byte> output) noexcept -> std::expected<void, MemErr>;

    [[nodiscard]] auto attach(MemLink& attachment, Perms perms) noexcept -> std::expected<void, MemErr>;
    auto protect(MemLink&, Perms) noexcept -> std::expected<void, MemErr>;
    [[nodiscard]] auto attachment_count() const noexcept -> usize;
    void retire(object::cleanup&& cleanup = {}) noexcept;

  private:
    friend struct object::traits<Mem>;
    friend class PageHold;
    friend class PageTransfer;
    friend class MemLink;
    friend class MemWork;
    friend class Cache;
    friend class ::Pager;
    friend class VSpace;

    struct AttachmentCompare {
        bool operator()(const MemLink& a, const MemLink& b) const noexcept { return a.id() < b.id(); }
        bool operator()(u64 a, const MemLink& b) const noexcept { return a < b.id(); }
        bool operator()(const MemLink& a, u64 b) const noexcept { return a.id() < b; }
    };
    using AttachmentTree = libk::intrusive_tree<MemLink, &MemLink::memory_hook_, AttachmentCompare>;

    // Adopts one operation already admitted under lock_. Its scope includes
    // foreign calls and rollback; releasing it may destroy this Mem.
    class Pin {
        Mem* mem_;

      public:
        explicit Pin(Mem& mem) noexcept : mem_(&mem) {}
        Pin(const Pin&) = delete;
        Pin(Pin&& p) noexcept : mem_(std::exchange(p.mem_, nullptr)) {}
        ~Pin() noexcept {
            if (mem_) mem_->drop_page();
        }
        void release() noexcept { mem_ = nullptr; }
    };

    [[nodiscard]] auto detach(MemLink& attachment) noexcept -> bool;
    template <class F> auto paged(F&& fn, MemErr error = MemErr::InvalidState) noexcept;
    void drop_page() noexcept;
    void drop_work() noexcept;
    void prepared() noexcept {
        sync::Lock guard{lock_}; libk_assert(preparing_);
        if (!--preparing_ && trim_op_) trim_op_->post();
    }
    void finish_transfer(usize page_index, OwnedPage&& page, bool commit) noexcept;
    void finish_retire(bool drop_request = false) noexcept;
    void release() noexcept;
    auto write(const PageHold&, usize, std::span<const byte>) noexcept -> std::expected<void, MemErr>;
    void finish_trim() noexcept;
    auto start_trim(ObjectRange, Source::Context&, PageHold&&, bool,
        cap::Graph::Admission, std::span<const object::ref<>*>, std::span<const cap::GrantPin*>, bool copy = false) noexcept -> std::expected<void, MemErr>;
    auto invalidate(ObjectRange, usize) noexcept -> bool;
    void stop() noexcept;
    void run_work() noexcept;
    void advance() noexcept;
    void post() noexcept { if (work_) work_->post(job_); }
    void request_pin() noexcept;
    void request_drop() noexcept;
    void cancel_request(Pager::Reply&) noexcept;
    void invalidated() noexcept { auto self = object::ref<>::retain_alive(job_.key()); static_cast<void>(self.destroy()); }
    cap::Bindings<2> backing_{Source::Action::bind<&Mem::invalidated>(*this), Source::Action::bind<&Mem::post>(*this)};
    object::ref<> quota_{};
    Pmm* pmm_{};
    usize logical_pages_{}, prepay_{};
    mutable sync::Spin lock_{};
    AttachmentTree attachments_{};
    MemLink* scan_{};
    std::optional<ObjectRange> trimming_{};
    Src<trim_clean | trim_dirty | trim_busy | trim_canceled> trim_events_;
    enum class TrimPhase : u8 { Invalidate, Wait, Pages, Restore, Finish };
    TrimPhase trim_phase_{};
    usize trim_cursor_{}, trim_released_{};
    bool trim_removing_{};
    status_t trim_status_{};
    std::optional<Op> trim_op_;
    bool advancing_{}, again_{};
    std::optional<Store> store_{};
    usize operations_{}, preparing_{}, trim_pending_{};
    bool reassigning_{};
    std::atomic<usize> request_pins_{};
    MemState state_{MemState::Live};
    object::cleanup cleanup_{};
    SealState seal_{SealState::Loadable};
    ContentEpoch content_epoch_{};
    Perms perms_{};
    object::ref<> pager_ref_{};
    Pager::Reservation reservation_;
    Pager::Binding binding_{};
    std::optional<PageHold> snapshot_;
    bool copying_{}, notify_reclaim_{};
    bool releasing_{}, stopping_{};
    WorkQueue* work_{};
    Work job_{Work::Fn::bind<&Mem::run_work>(*this)};
    status_t init_status_{STATUS_PENDING};
    Op initialization_{{}, Source::Action::bind<&Mem::post>(*this), Source::Action::bind<&Mem::advance>(*this)};
    Work ordinary_{Work::Fn::bind<&Mem::advance>(*this), Work::Fn::bind<&Mem::post>(*this)};
    // Closes page request admission before retirement scans the backing.
    std::atomic<bool> work_open_{true};
};

} // namespace mm
