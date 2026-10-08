#pragma once

#include <libk/unique_handle.hpp>

#include <base/types.hpp>
#include <cpu/types.hpp>
#include <expected>
#include <libk/intrusive_list.hpp>
#include <libk/noncopyable.hpp>
#include <libk/span.hpp>
#include <libk/sync/atomic.hpp>
#include <limits>
#include <mm/pmm.hpp>
#include <optional>
#include <sync.hpp>
#include <utility>
#include <variant>
#include <libk/intrusive_tree.hpp>
#include <wait.hpp>

class Thread;
class Cpus;
#include <mm/pager.hpp>

#include <mm/types.hpp>
#include <object/ref.hpp>
#include <resource/sponsorship.hpp>

namespace object {
template <typename T> struct traits;
}

namespace mm {

class Mem;
class PageReq;

enum class WaitPhase : u8 {
    Detached,
    Attached,
    Publishing,
};

enum class WaitRc : u8 {
    Ready,
    Failed,
    Canceled,
    Dirty,
};

struct WaitQueue;
class WaitClaim;

// Stack-owned content request. Claiming finalizes the relation before its
// callback, so a waking caller may immediately reuse its storage.
struct WaitRelation final {
    using Publish = void (*)(void*, WaitRc) noexcept;

    libk::IntrusiveListHook hook_{};
    WaitQueue* request{};
    void* owner{};
    Publish publish{};
    bool (*arm)(void*) noexcept {};
    u64 generation{};
    libk::Atomic<u8> state_{static_cast<u8>(WaitPhase::Detached)};

    [[nodiscard]] auto attached() const noexcept -> bool {
        const auto state = static_cast<WaitPhase>(state_.load<libk::MemoryOrder::Acquire>());
        return state != WaitPhase::Detached;
    }
    [[nodiscard]] auto state() const noexcept -> WaitPhase {
        return static_cast<WaitPhase>(state_.load<libk::MemoryOrder::Acquire>());
    }

};

// One terminal callback obligation. Finalization detaches host storage;
// publish consumes the snapshot before invoking foreign code.
class WaitClaim final : private libk::noncopyable {
    struct Data {
        void* owner{};
        WaitRelation::Publish publish{};
        WaitRc result{WaitRc::Canceled};
    } d_{};

  public:
    WaitClaim() noexcept = default;
    WaitClaim(WaitClaim&& other) noexcept : d_(std::exchange(other.d_, {})) {}
    auto operator=(WaitClaim&& other) noexcept -> WaitClaim& {
        if (this != &other) {
            libk_assert(!*this);
            d_ = std::exchange(other.d_, {});
        }
        return *this;
    }
    ~WaitClaim() noexcept { libk_assert(!*this); }
    [[nodiscard]] explicit operator bool() const noexcept { return d_.publish; }
    [[nodiscard]] auto publish() noexcept -> bool;

  private:
    friend struct WaitQueue;
    WaitClaim(void* owner, WaitRelation::Publish publish, WaitRc rc) noexcept
        : d_{owner, publish, rc} {}
};

struct WaitQueue final {
    using List = libk::IntrusiveList<WaitRelation, &WaitRelation::hook_>;
    List waiters{};
    [[nodiscard]] auto attach(WaitRelation&, void*, WaitRelation::Publish) noexcept -> bool;
    [[nodiscard]] auto detach(WaitRelation&, u64 generation) noexcept -> bool;
    // Claim this request's complete window under the host lock. Later arrivals
    // belong to the next request; cancellation cannot remove Publishing nodes.
    [[nodiscard]] auto take() noexcept -> List;
    // Detach one claimed relation under the same lock before its callback runs.
    [[nodiscard]] static auto finish(List&, WaitRc) noexcept -> WaitClaim;
};

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
};

enum class ContentState : u8 {
    Zero,
    Resident,
    Busy,
    Failed,
};

struct Frame final {
    Page page{};
    Perms perms{};
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
    struct Data {
        MemLink* attachment{};
        Mem* pin{};
        ObjectRange range{};
        static auto empty() noexcept -> Data { return {}; }
        static bool is_empty(const Data& d) noexcept { return !d.attachment; }
    };
    struct Drop {
        void operator()(Data&) const noexcept;
    };
    explicit MemWork(MemLink& a, Mem* pin = nullptr, ObjectRange r = {}) noexcept : h_(Data{&a, pin, r}) {}
    libk::unique_handle<Data, Drop, Data> h_{};
};

struct MemOps final {
    void (*invalidate)(void* context, MemWork&& work) noexcept;
    void (*released)(void* context) noexcept;
};

// Embedded in a Map and indexed non-owningly by Mem. The Map
// must retain its structural Mem reference until detach() completes.
class MemLink final : private libk::noncopyable_nonmovable {
  public:
    MemLink(void* context, const MemOps& ops) noexcept : context_(context), ops_(&ops) {}
    ~MemLink() noexcept;

    [[nodiscard]] auto attached() const noexcept -> bool;
    [[nodiscard]] auto busy() const noexcept -> bool;
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
    };

    void drop_work() noexcept;

    libk::IntrusiveListHook memory_hook_{};
    Mem* owner_{};
    void* context_{};
    const MemOps* ops_{};
    libk::Atomic<usize> work_{};
    libk::Atomic<u8> state_{static_cast<u8>(State::Idle)};
    Perms perms_{};
};

// Owns one Mem operation pin through the caller or installed PTE lifetime.
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
    PageHold(Mem& owner, Frame frame) noexcept : h_(Data{&owner, frame}) {}

    struct Data {
        Mem* owner{};
        Frame frame{};
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
    WaitQueue waiters;
    u64 dirty_epoch{}, usage_epoch{};
    bool failed{}, write_failed{};
    ~PagerData() noexcept { libk_assert(!request.pager && waiters.waiters.empty()); }
};

template<class Extra = std::monostate> class Cache {
    static constexpr bool is_paged = std::same_as<Extra, PagerData>;
    struct Node : Extra {
        explicit Node(usize i) noexcept : index(i) {}
        usize index;
        resource::Charge charge;
        OwnedPage resident;
        libk::IntrusiveTreeHook hook;
    };
    struct Compare {
        bool operator()(const Node& a, const Node& b) const noexcept { return a.index < b.index; }
        bool operator()(usize a, const Node& b) const noexcept { return a < b.index; }
        bool operator()(const Node& a, usize b) const noexcept { return a.index < b; }
    };
    struct Drop { Cache* cache; void operator()(Node*& n) noexcept { cache->rows_.destroy(*n); } };
    using Row = libk::unique_handle<Node*, Drop>;
    struct PagerCfg { Pager* pager; bool priv; };
  public:
    Cache(Pmm&, Perms, Pager* = nullptr, bool = false) noexcept;
    Cache(const Cache&) = delete;
    Cache(Cache&&) noexcept;
    ~Cache() noexcept;
    auto query(usize) const noexcept -> ContentState;
    auto materialize(Mem&, usize, WaitRelation*, void*, WaitRelation::Publish) noexcept
        -> std::expected<Frame, MemErr>;
    auto allocate(const object::ref<>&, usize) noexcept -> std::expected<Frame, MemErr> requires (!is_paged);
    auto begin_transfer(usize) noexcept -> std::expected<OwnedPage, MemErr> requires (!is_paged);
    auto restore_transfer(usize, OwnedPage&&) noexcept -> std::expected<void, MemErr> requires (!is_paged);
    auto commit_transfer(usize) noexcept -> std::expected<void, MemErr> requires (!is_paged);
    void stop(Mem&) noexcept requires is_paged;
    auto cancel_fault(WaitRelation&, u64) noexcept -> bool requires is_paged;
    auto supply(Mem&, Pager&, u64, OwnedPage&&) noexcept -> std::expected<void, MemErr> requires is_paged;
    auto finish(Mem&, Pager&, u64, bool) noexcept -> std::expected<void, MemErr> requires is_paged;
    void complete(Pager::Reply&, bool) noexcept requires is_paged;
    auto observe_usage(usize, bool, bool) noexcept -> std::expected<void, MemErr> requires is_paged;
    auto writeback(usize) noexcept -> std::expected<void, MemErr> requires is_paged;
    auto trim(ObjectRange) noexcept -> std::expected<void, MemErr> requires is_paged;
  private:
    auto make(const object::ref<>&, usize) noexcept -> std::expected<Row, MemErr>;
    static auto charge_page(const object::ref<>&) noexcept -> std::expected<resource::Charge, MemErr>;
    void publish_waiters(Node&, WaitRc) noexcept requires is_paged;
    auto writeback_locked(Node&) noexcept -> bool requires is_paged;
    Pmm* pmm_;
    Perms perms_;
    mutable sync::Spin tree_lock_;
    libk::IntrusiveTree<Node, &Node::hook, Compare> nodes_;
    Slab<Node, false, false> rows_;
    [[no_unique_address]] std::conditional_t<is_paged, PagerCfg, std::monostate> cfg_{};
};
using Anon = Cache<>;
using Paged = Cache<PagerData>;

class Extents {
    struct Row { explicit Row(Extent e) noexcept : extent(e) {} Extent extent; Row* next{}; };
  public:
    explicit Extents(Pmm&) noexcept;
    Extents(const Extents&) = delete;
    Extents(Extents&&) noexcept;
    ~Extents() noexcept;
    auto initialize(const object::ref<>&, libk::Span<const Extent>, PageGroup&&) noexcept
        -> std::expected<void, MemErr>;
    auto query(usize) const noexcept -> ContentState;
    auto materialize(usize) const noexcept -> std::expected<Frame, MemErr>;
  private:
    auto find(usize) const noexcept -> const Extent*;
    void reset() noexcept;
    Slab<Row, false> rows_;
    Row* head_{};
    PageGroup owned_{};
};

struct PhysCfg { libk::Span<const Extent> extents; PageGroup owned; };
struct PagedCfg { object::ref<> pager; Perms perms; bool priv{}; };

class Mem final : private libk::noncopyable_nonmovable {
    using Store = std::variant<Anon, Paged, Extents>;
  public:
    using Config = std::variant<AnonCfg, PhysCfg, PagedCfg>;
    class Data {
        friend class Mem;
        Data(Pmm& pmm, usize pages) noexcept : pmm_(&pmm), pages_(pages) {}
        object::ref<> payer_, pager_;
        Pmm* pmm_;
        usize pages_;
        Perms perms_;
        std::optional<Store> store_;
      public:
        Data(Data&&) noexcept = default;
        Data(const Data&) = delete;
    };
    static auto prepare(const object::ref<>& payer, Pmm&, usize, Config) noexcept
        -> std::expected<Data, MemErr>;
    explicit Mem(Data&&) noexcept;
    ~Mem() noexcept;

    [[nodiscard]] auto size() const noexcept -> usize { return logical_pages_ * page_size; }
    [[nodiscard]] auto page_count() const noexcept -> usize { return logical_pages_; }
    [[nodiscard]] auto kind() const noexcept -> BackingKind;
    [[nodiscard]] auto state() const noexcept -> MemState;
    [[nodiscard]] auto seal_state() const noexcept -> SealState;
    [[nodiscard]] auto content_epoch() const noexcept -> ContentEpoch;
    // Publishes immutable executable content. The synchronous E0 path only
    // succeeds once no writable mapping attachment remains; later async
    // callers use the same state transition after retiring those mappings.
    [[nodiscard]] auto seal() noexcept -> std::expected<void, MemErr>;
    [[nodiscard]] auto query(usize page_index) const noexcept -> std::expected<ContentState, MemErr>;
    [[nodiscard]] auto populate(Thread&, Cpus&, usize page) noexcept -> std::expected<void, MemErr>;
    [[nodiscard]] auto materialize(usize page_index) noexcept -> std::expected<PageHold, MemErr>;
    [[nodiscard]] auto materialize(usize page_index, WaitRelation* relation, void* owner,
                                   WaitRelation::Publish publish) noexcept -> std::expected<PageHold, MemErr>;
    [[nodiscard]] auto begin_transfer(usize page_index) noexcept -> std::expected<PageTransfer, MemErr>;
    [[nodiscard]] auto supply(Pager& pager, u64 id, OwnedPage&& page) noexcept -> std::expected<void, MemErr>;
    [[nodiscard]] auto supply(Pager& pager, PageTransfer&& page, u64 id) noexcept
        -> std::expected<void, MemErr>;
    [[nodiscard]] auto pager_finish(Pager& pager, u64 id, bool fail) noexcept -> std::expected<void, MemErr>;
    [[nodiscard]] auto observe_usage(usize page_index, bool accessed, bool dirty) noexcept
        -> std::expected<void, MemErr>;
    [[nodiscard]] auto trim(ObjectRange, WaitRelation&, void*, WaitRelation::Publish) noexcept
        -> std::expected<void, MemErr>;
    [[nodiscard]] auto trim(Thread&, Cpus&, ObjectRange) noexcept -> std::expected<void, MemErr>;
    [[nodiscard]] auto writeback(usize page_index) noexcept -> std::expected<void, MemErr>;
    // Initialize a private anonymous page without publishing a CPU mapping.
    // A write is bounded to one page and excludes attachments and page loans.
    [[nodiscard]] auto write(usize offset, libk::Span<const byte> input) noexcept
        -> std::expected<void, MemErr>;
    [[nodiscard]] auto read(usize offset, libk::Span<byte> output) noexcept -> std::expected<void, MemErr>;

    [[nodiscard]] auto attach(MemLink& attachment, Perms perms) noexcept -> std::expected<void, MemErr>;
    [[nodiscard]] auto attachment_count() const noexcept -> usize;
    void retire(object::cleanup&& cleanup = {}) noexcept;

  private:
    friend struct object::traits<Mem>;
    friend class PageHold;
    friend class PageTransfer;
    friend class MemLink;
    friend class MemWork;
    template<class> friend class Cache;
    friend class ::Pager;
    friend class ::Pager::Claims;
    friend class PageReq;
    friend class VSpace;

    using AttachmentList = libk::IntrusiveList<MemLink, &MemLink::memory_hook_>;

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

    [[nodiscard]] auto materialize_impl(usize page_index, WaitRelation* relation, void* owner,
                                        WaitRelation::Publish publish) noexcept
        -> std::expected<PageHold, MemErr>;
    void release_fault() noexcept;
    [[nodiscard]] auto cancel_fault(WaitRelation& relation, u64 generation) noexcept -> bool;
    [[nodiscard]] auto detach(MemLink& attachment) noexcept -> bool;
    template <class F> auto paged(F&& fn, MemErr error = MemErr::InvalidState) noexcept;
    void drop_page() noexcept;
    void finish_transfer(usize page_index, OwnedPage&& page, bool commit) noexcept;
    void finish_retire(bool drop_request = false) noexcept;
    void finish_trim() noexcept;
    void invalidate(ObjectRange) noexcept;
    void stop() noexcept;
    void request_pin() noexcept;
    void request_drop() noexcept;
    void cancel_request(Pager::Reply&) noexcept;
    object::ref<> payer_{};
    Pmm* pmm_{};
    usize logical_pages_{};
    mutable sync::Spin lock_{};
    AttachmentList attachments_{};
    std::optional<ObjectRange> trimming_{};
    WaitQueue trim_waiters_{};
    bool trim_walk_{};
    std::optional<Store> store_{};
    usize operations_{};
    libk::Atomic<usize> request_pins_{};
    MemState state_{MemState::Live};
    object::cleanup cleanup_{};
    SealState seal_{SealState::Loadable};
    ContentEpoch content_epoch_{};
    Perms perms_{};
    object::ref<> pager_ref_{};
    bool releasing_{};
    // Closes page request admission before retirement scans the backing.
    libk::Atomic<bool> work_open_{true};
};

// One content completion, owned by the blocked call's stack.
// The host installs the thread edge under its lock before linking this request.
class PageReq final : private libk::noncopyable_nonmovable {
  public:
    PageReq(Thread&, Cpus&) noexcept;
    auto wait(Mem* = nullptr) noexcept -> WaitRc;
    static void publish(void*, WaitRc) noexcept;
    WaitRelation relation;

  private:
    void release() noexcept;
    auto cancel() noexcept -> bool;
    static auto arm(void*) noexcept -> bool;
    Thread& thread_;
    Cpus& cpus_;
    Completion done_;
    Mem* mem_{};
    WaitRc result_{WaitRc::Ready};
};

} // namespace mm
