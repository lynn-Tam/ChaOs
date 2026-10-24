#pragma once

#include <stddef.h>
#include <atomic>
#include <stdint.h>

#include <libk/types.hpp>
#include <expected>
#include <libk/inplace_vector.hpp>
#include <libk/manual_lifetime.hpp>
#include <libk/memory.hpp>
#include <libk/noncopyable.hpp>
#include <libk/unique_handle.hpp>
#include <limits>
#include <mm/types.hpp>
#include <libk/checked_arithmetic.hpp>
#include <ranges>
#include <span>
#include <optional>
#include <sync.hpp>
#include <type_traits>

namespace mm {

enum class PageState : uint8_t {
    Reserved,
    Free,
    Allocated,
};

enum class PmmInitError : uint8_t {
    EmptyMemoryMap,
    InvalidRegion,
    OverlappingRegions,
    NoRam,
    MetadataOverflow,
    NoMetadataStorage,
    OutsideWindow,
};

// Convert firmware RAM banks and overlapping reservations into one inventory.
// MMIO may not overlap RAM; Boot/Kernel ownership must be backed by RAM.
auto normalize(RegionList&) noexcept -> std::expected<void, PmmInitError>;

enum class AllocError : uint8_t {
    NoMemory,
};

enum class QueryError : uint8_t {
    NotManaged,
};

enum class BootErr : uint8_t {
    WrongOwner,
    InvalidReservation,
};

struct PmmStats {
    size_t arena_count{};
    size_t metadata_pages{};
    size_t boot_reservations{};
    size_t reserved_pages{};
    size_t free_pages{};
    size_t allocated_pages{};
};

class Pmm;
class PageGroup;

class OwnedPage {
  public:
    OwnedPage() noexcept = default;
    OwnedPage(const OwnedPage&) = delete;
    auto operator=(const OwnedPage&) -> OwnedPage& = delete;

    OwnedPage(OwnedPage&&) noexcept = default;
    auto operator=(OwnedPage&&) noexcept -> OwnedPage& = default;
    ~OwnedPage() noexcept = default;

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] auto page() const noexcept -> Page;
    [[nodiscard]] auto bytes() noexcept -> byte*;
    [[nodiscard]] auto bytes() const noexcept -> const byte*;
    void reset() noexcept { h_.reset(); }

  private:
    friend class Pmm;
    friend class PageGroup;

    OwnedPage(Pmm& owner, Page page, uint32_t generation) noexcept;
    auto disarm() noexcept -> void;

    struct Data {
        Pmm* owner{};
        Page page{};
        u32 gen{};
        static auto empty() noexcept -> Data { return {}; }
        static bool is_empty(const Data& d) noexcept { return !d.owner; }
    };
    struct Drop {
        void operator()(Data& d) const noexcept;
    };
    libk::unique_handle<Data, Drop, Data> h_{};
};

// Exclusive claim on a boot region. PMM owns the range and its state;
// the handle owns only the issued claim and returns it on destruction.
class BootPages {
  public:
    BootPages(BootPages&&) noexcept = default;
    auto operator=(BootPages&&) noexcept -> BootPages& = default;
    explicit operator bool() const noexcept { return bool(h_); }
    auto range() const noexcept -> Pages;
    void reset() noexcept { h_.reset(); }

  private:
    friend class Pmm;
    struct Data {
        Pmm* owner{};
        usize id{};
        static auto empty() noexcept -> Data { return {}; }
        static bool is_empty(const Data& d) noexcept { return !d.owner; }
    };
    struct Drop {
        void operator()(Data& d) const noexcept;
    };
    BootPages(Pmm& pmm, usize id) noexcept : h_(Data{&pmm, id}) {}
    libk::unique_handle<Data, Drop, Data> h_;
};

class Pmm {
    class ConstructionKey {
        friend class Pmm;
        constexpr ConstructionKey() noexcept = default;
    };

  public:
    using InitializationResult = std::expected<void, PmmInitError>;
    using AllocateResult = std::expected<OwnedPage, AllocError>;
    using GroupAllocateResult = std::expected<Page, AllocError>;
    using QueryResult = std::expected<PageState, QueryError>;
    using ReclaimResult = std::expected<size_t, BootErr>;
    using AdoptResult = std::expected<PageGroup, BootErr>;

    struct Window { pa_t pa{}; va_t va{}; usize size{}; };
    [[nodiscard]] static auto initialize_in(libk::manual_lifetime<Pmm>& storage, RegionList&& memory_map,
                                            Window layout) noexcept -> InitializationResult;

    explicit Pmm([[maybe_unused]] ConstructionKey key, RegionList&& memory, Window layout) noexcept
        : memory_(std::move(memory)),
          window_(layout) {}

    Pmm(const Pmm&) = delete;
    auto operator=(const Pmm&) -> Pmm& = delete;
    Pmm(Pmm&& other) = delete;
    auto operator=(Pmm&&) -> Pmm& = delete;
    ~Pmm() noexcept;

    [[nodiscard]] auto allocate_page() noexcept -> AllocateResult;
    [[nodiscard]] auto group() noexcept -> PageGroup;
    [[nodiscard]] auto take_boot() noexcept -> std::optional<BootPages>;
    [[nodiscard]] auto take_boot(Pages range) noexcept -> std::optional<BootPages>;
    [[nodiscard]] auto reclaim(BootPages&& reservation) noexcept -> ReclaimResult;
    [[nodiscard]] auto adopt(BootPages&& reservation) noexcept -> AdoptResult;

    [[nodiscard]] auto contains(Page page) const noexcept -> bool;
    [[nodiscard]] auto state_of(Page page) const noexcept -> QueryResult;
    // All managed frames, including free, allocated and reserved pages.
    [[nodiscard]] auto page_count() const noexcept -> size_t;
    [[nodiscard]] auto free_page_count() const noexcept -> size_t;
    [[nodiscard]] auto arena_count() const noexcept -> size_t;
    [[nodiscard]] auto metadata_page_count() const noexcept -> size_t;
    bool covers(Pages range) const noexcept;
    bool is_ram(Pages range) const noexcept;
    bool is_ram(Page p) const noexcept { return is_ram(Pages{p, 1}); }
    auto ram() const noexcept {
        return regions() | std::views::filter([](const Region& r) { return r.is_ram(); }) |
               std::views::transform([](const Region& r) { return r.range; });
    }
    auto virt(pa_t pa, usize size) const noexcept -> std::optional<va_t>;
    auto phys(va_t va, usize size) const noexcept -> std::optional<pa_t>;
    template<class T> auto ptr(pa_t pa, usize count = 1) const noexcept -> std::optional<T*> {
        const auto size = libk::checked_multiply(sizeof(T), count);
        const auto va = size ? virt(pa, *size) : std::nullopt;
        return va ? std::optional<T*>{reinterpret_cast<T*>(va->raw())} : std::nullopt;
    }
    auto regions() const noexcept -> std::span<const Region> { return {memory_.data(), memory_.size()}; }
    [[nodiscard]] auto bytes(Page page) noexcept -> byte*;
    [[nodiscard]] auto bytes(Page page) const noexcept -> const byte*;
    [[nodiscard]] auto stats() const noexcept -> PmmStats;
    [[nodiscard]] auto verify_invariants() const noexcept -> bool;

  private:
    friend class OwnedPage;
    friend class PageGroup;
    friend class BootPages;

    static constexpr usize Nil = ~usize{};
    enum class State : u8 { Reserved, Free, Single, Group };

    struct Desc {
        // One publication word proves ownership without taking the allocator lock.
        // Group ids never repeat; callers serialize the group lifetime itself.
        std::atomic<usize> tag;
        u32 gen{};
        usize next{Nil}, prev{Nil};
        static constexpr auto label(State state, usize owner) noexcept -> usize {
            return ((owner + 1) << 2) | static_cast<usize>(state);
        }
        Desc(State state, u32 gen = 0, usize next = Nil, usize owner = Nil) noexcept
            : tag(label(state, owner)), gen(gen), next(next) {}
        Desc(const Desc& other) noexcept : tag(other.tag.load(std::memory_order_relaxed)), gen(other.gen), next(other.next), prev(other.prev) {}
        auto operator=(const Desc& other) noexcept -> Desc& {
            gen = other.gen; next = other.next; prev = other.prev;
            tag.store(other.tag.load(std::memory_order_relaxed), std::memory_order_release);
            return *this;
        }
        auto state() const noexcept -> State { return static_cast<State>(tag.load(std::memory_order_relaxed) & 3); }
        auto owner() const noexcept -> usize { return (tag.load(std::memory_order_relaxed) >> 2) - 1; }
        auto owns(usize owner) const noexcept -> bool {
            return tag.load(std::memory_order_acquire) == label(State::Group, owner);
        }
        void relabel(usize owner) noexcept { tag.store(label(State::Group, owner), std::memory_order_release); }
        static auto reserved(usize owner = Nil, u32 gen = 0) noexcept -> Desc { return {State::Reserved, gen, Nil, owner}; }
        static auto free(u32 gen = 0, usize next = Nil) noexcept -> Desc { return {State::Free, gen, next}; }
        static auto individual(u32 gen) noexcept -> Desc { return {State::Single, gen}; }
        static auto group(usize owner, usize next, u32 gen) noexcept -> Desc { return {State::Group, gen, next, owner}; }
    };
    static_assert(sizeof(Desc) == 32);

    struct Arena {
        Pages range{};
        Pages descriptor_storage{};
        usize free_head{Nil};
        size_t free_count{};
    };

    enum class ReservationState : uint8_t {
        Available,
        Issued,
        Consumed,
    };

    struct ReservationRecord {
        Pages range{};
        ReservationState state{ReservationState::Available};
    };

    [[nodiscard]] auto initialize() noexcept -> InitializationResult;
    auto descriptor(usize id) noexcept -> Desc&;
    [[nodiscard]] auto descriptor_at(Arena& arena, usize index) noexcept -> Desc&;
    [[nodiscard]] auto descriptor_at(const Arena& arena, usize index) const noexcept -> const Desc&;
    [[nodiscard]] static auto page_at(const Arena& arena, usize index) noexcept -> Page;
    [[nodiscard]] static auto index_of(const Arena& arena, Page page) noexcept -> usize;

    [[nodiscard]] static auto public_state_of(State state) noexcept -> PageState;
    [[nodiscard]] static auto global_frame_id_of(Page page) noexcept -> usize;
    [[nodiscard]] static auto page_from(usize id) noexcept -> Page;
    [[nodiscard]] auto next_group_id() noexcept -> usize;
    [[nodiscard]] auto find_arena(Page page) noexcept -> Arena*;
    [[nodiscard]] auto find_arena(Page page) const noexcept -> const Arena*;
    auto push_free(Arena& arena, usize index) noexcept -> void;
    [[nodiscard]] auto pop_free(Arena& arena) noexcept -> usize;
    [[nodiscard]] auto allocate_page_into(PageGroup& group) noexcept -> GroupAllocateResult;
    auto grow(PageGroup&, usize) noexcept -> bool;
    void append(PageGroup& dst, PageGroup& src) noexcept;
    [[nodiscard]] auto detach_page(PageGroup& group, Page page) noexcept -> std::optional<OwnedPage>;
    [[nodiscard]] auto detach_group_head(PageGroup& group) noexcept -> std::optional<OwnedPage>;
    [[nodiscard]] auto attach_page(PageGroup& group, OwnedPage& page) noexcept -> bool;
    [[nodiscard]] auto group_contains(const PageGroup& group, Page page) const noexcept -> bool;
    auto release(Page page, uint32_t generation) noexcept -> void;
    auto release_group_head(PageGroup& group) noexcept -> void;
    auto release(PageGroup& group) noexcept -> void;
    auto boot_range(usize id) const noexcept -> Pages;
    void cancel(usize id) noexcept;
    [[nodiscard]] auto verify(const Arena& arena) const noexcept -> bool;
    [[nodiscard]] auto verify_invariants_unlocked() const noexcept -> bool;

    mutable sync::Spin lock_{};
    RegionList memory_{};
    Window window_{};
    libk::inplace_vector<Arena, max_regions> arenas_{};
    libk::inplace_vector<ReservationRecord, max_regions> reservations_{};
    size_t outstanding_pages_{};
    size_t outstanding_group_pages_{};
    size_t outstanding_groups_{};
    size_t next_group_id_{};
    size_t issued_reservations_{};
};

// The caller serializes this owner; Pmm serializes descriptor mutations.
class PageGroup {
  public:
    PageGroup() noexcept = default;
    PageGroup(const PageGroup&) = delete;
    auto operator=(const PageGroup&) -> PageGroup& = delete;

    PageGroup(PageGroup&& other) noexcept;
    auto operator=(PageGroup&& other) noexcept -> PageGroup&;
    ~PageGroup() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] auto page_count() const noexcept -> size_t;
    [[nodiscard]] auto owner() const noexcept -> Pmm&;
    [[nodiscard]] auto contains(Page page) const noexcept -> bool;
    [[nodiscard]] auto bytes(Page page) noexcept -> byte*;
    [[nodiscard]] auto bytes(Page page) const noexcept -> const byte*;

    [[nodiscard]] auto allocate() noexcept -> Pmm::GroupAllocateResult;
    // Caller exclusively owns both groups. Transfers the source chain,
    // relabelling descriptors in O(n) outside the global PMM lock.
    void append(PageGroup&&) noexcept;
    [[nodiscard]] auto grow(size_t page_count) noexcept -> bool;
    // Transfers one page out of the group without releasing the frame.  This
    // is the ownership primitive used by translation detach/retire batches.
    [[nodiscard]] auto detach(Page page) noexcept -> std::optional<OwnedPage>;
    // Transfers the current group head to individual ownership in O(1).
    // Prepared page-table reserves can consume a batch without maintaining a
    // second list of physical page identities.
    [[nodiscard]] auto take() noexcept -> std::optional<OwnedPage>;
    // Regroups an individually owned frame without freeing or reallocating it.
    // Retirement batches use this to keep an unbounded set of detached table
    // pages behind one RAII owner.
    [[nodiscard]] auto attach(OwnedPage&& page) noexcept -> bool;
    auto reset() noexcept -> void;

  private:
    friend class Pmm;
    PageGroup(Pmm& owner, usize id) noexcept;

    auto disarm() noexcept -> void;

    libk::observer_ptr<Pmm> owner_{};
    usize head_{Pmm::Nil};
    size_t page_count_{};
    usize id_{Pmm::Nil};
};

static_assert(sizeof(PageGroup) == 32);

} // namespace mm
