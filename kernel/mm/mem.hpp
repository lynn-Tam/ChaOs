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
#include <wait.hpp>

class Thread;
class CpuRegistry;
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
    Perms access{};
};

struct AnonCfg final {
    Perms access{Perms::of(Perm::Read, Perm::Write)};
    bool eager{};
};

enum class BootOwnership : u8 {
    Borrowed,
    Owned,
};

enum class BackingKind : u8 {
    Anonymous,
    Physical,
    Boot,
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
    Perms access{};
    MemoryType type{MemoryType::Normal};
};

class Mem;
class MemWork;

enum class MemErr : u8 {
    InvalidSize,
    InvalidRange,
    InvalidAccess,
    InvalidMemoryType,
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
    Perms access_{};
};

// Owns one Mem operation pin through the caller or installed PTE lifetime.
class PageHold final : private libk::noncopyable {
  public:
    PageHold() noexcept = default;
    PageHold(PageHold&&) noexcept = default;
    auto operator=(PageHold&&) noexcept -> PageHold& = default;
    ~PageHold() noexcept = default;

    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(h_); }
    [[nodiscard]] auto page() const noexcept -> Frame { return h_.get().page; }
    void reset() noexcept { h_.reset(); }

  private:
    friend class Mem;
    PageHold(Mem& owner, Frame page) noexcept : h_(Data{&owner, page}) {}

    struct Data {
        Mem* owner{};
        Frame page{};
        static auto empty() noexcept -> Data { return {}; }
        static bool is_empty(const Data& d) noexcept { return !d.owner; }
    };
    struct Drop {
        void operator()(Data& d) const noexcept;
    };
    libk::unique_handle<Data, Drop, Data> h_{};
};

class Mem final : private libk::noncopyable_nonmovable {
  public:
    Mem(Pmm& pmm, usize byte_size) noexcept;
    ~Mem() noexcept;

    [[nodiscard]] auto init_anon(AnonCfg config) noexcept -> std::expected<void, MemErr>;
    [[nodiscard]] auto init_phys(libk::Span<const Extent> extents) noexcept -> std::expected<void, MemErr>;
    [[nodiscard]] auto init_boot(libk::Span<const Extent> extents, BootOwnership ownership,
                                 PageGroup&& owned = {}) noexcept -> std::expected<void, MemErr>;
    // The structural reference keeps the Pager payload alive for the whole
    // backing lifetime.
    [[nodiscard]] auto init_paged(object::ref<>&& pager, Perms access, bool private_content = false) noexcept
        -> std::expected<void, MemErr>;

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
    [[nodiscard]] auto populate(Thread&, CpuRegistry&, usize page) noexcept -> std::expected<void, MemErr>;
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
    [[nodiscard]] auto trim(Thread&, CpuRegistry&, ObjectRange) noexcept -> std::expected<void, MemErr>;
    [[nodiscard]] auto writeback(usize page_index) noexcept -> std::expected<void, MemErr>;
    // Initialize a private anonymous page without publishing a CPU mapping.
    // A write is bounded to one page and excludes attachments and page loans.
    [[nodiscard]] auto write(usize offset, libk::Span<const byte> input) noexcept
        -> std::expected<void, MemErr>;
    [[nodiscard]] auto read(usize offset, libk::Span<byte> output) noexcept -> std::expected<void, MemErr>;

    [[nodiscard]] auto attach(MemLink& attachment, Perms access) noexcept -> std::expected<void, MemErr>;
    [[nodiscard]] auto attachment_count() const noexcept -> usize;
    void retire(object::cleanup&& cleanup = {}) noexcept;

  private:
    friend struct object::traits<Mem>;
    friend class PageHold;
    friend class PageTransfer;
    friend class MemLink;
    friend class MemWork;
    friend class Paged;
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

    struct Store;

    [[nodiscard]] auto initialize_backing(BackingKind kind, libk::Span<const Extent> extents,
                                          AnonCfg anonymous, BootOwnership boot_ownership,
                                          PageGroup&& boot_pages, Pager* pager, Perms pager_access,
                                          object::ref<>&& pager_ref, bool private_content = false) noexcept
        -> std::expected<void, MemErr>;
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
    void fail_build() noexcept;
    void bind_sponsor(resource::Sponsorship& sponsor) noexcept;
    [[nodiscard]] auto reserve_dynamic(resource::budget charge) noexcept
        -> std::expected<resource::Reservation, MemErr>;
    void request_pin() noexcept;
    void request_drop() noexcept;
    void cancel_request(Pager::Reply&) noexcept;
    Pmm* pmm_{};
    usize logical_pages_{};
    mutable sync::Spin lock_{};
    AttachmentList attachments_{};
    std::optional<ObjectRange> trimming_{};
    WaitQueue trim_waiters_{};
    bool trim_walk_{};
    Store* store_{};
    OwnedPage backing_page_{};
    resource::Sponsorship backing_sponsorship_{};
    usize operations_{};
    libk::Atomic<usize> request_pins_{};
    MemState state_{MemState::Building};
    object::cleanup cleanup_{};
    SealState seal_{SealState::Loadable};
    ContentEpoch content_epoch_{};
    Perms access_{};
    object::ref<> pager_ref_{};
    bool releasing_{};
    // Closes page request admission before retirement scans the backing.
    libk::Atomic<bool> work_open_{true};
    resource::Sponsorship* sponsor_{};
};

// One content completion, owned by the blocked call's stack.
// The host installs the thread edge under its lock before linking this request.
class PageReq final : private libk::noncopyable_nonmovable {
  public:
    PageReq(Thread&, CpuRegistry&) noexcept;
    auto wait(Mem* = nullptr) noexcept -> WaitRc;
    static void publish(void*, WaitRc) noexcept;
    WaitRelation relation;

  private:
    void release() noexcept;
    auto cancel() noexcept -> bool;
    static auto arm(void*) noexcept -> bool;
    Thread& thread_;
    CpuRegistry& cpus_;
    Completion done_;
    Mem* mem_{};
    WaitRc result_{WaitRc::Ready};
};

} // namespace mm
