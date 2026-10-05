#pragma once

#include <stddef.h>
#include <stdint.h>

#include <base/slab.hpp>
#include <base/types.hpp>
#include <expected>
#include <libk/inplace_vector.hpp>
#include <libk/manual_lifetime.hpp>
#include <libk/memory.hpp>
#include <libk/noncopyable.hpp>
#include <libk/unique_handle.hpp>
#include <limits>
#include <mm/phys.hpp>
#include <optional>
#include <resource/sponsorship.hpp>
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
    BadAttr,
    OverlappingRegions,
    NoRam,
    MetadataOverflow,
    NoMetadataStorage,
    OutsideWindow,
};

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

    [[nodiscard]] static auto initialize_in(libk::ManualLifetime<Pmm>& storage, RegionList&& memory_map,
                                            DirectMap::Layout layout) noexcept -> InitializationResult;

    explicit Pmm([[maybe_unused]] ConstructionKey key, RegionList&& memory, DirectMap::Layout layout) noexcept
        : memory_(std::move(memory)),
          direct_map_(std::span<const Region>{memory_.data(), memory_.size()}, layout) {}

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
    [[nodiscard]] auto direct_map(this auto& self) noexcept -> decltype(auto) { return (self.direct_map_); }
    auto attr_of(Pages range) const noexcept -> std::optional<CpuAttr>;
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
        State state{State::Reserved};
        u32 gen{};
        usize next{Nil};
        usize owner{Nil};

        static constexpr auto reserved(usize owner = Nil, u32 gen = 0) noexcept -> Desc {
            return {State::Reserved, gen, Nil, owner};
        }
        static constexpr auto free(u32 gen = 0, usize next = Nil) noexcept -> Desc {
            return {State::Free, gen, next};
        }
        static constexpr auto individual(u32 gen) noexcept -> Desc { return {State::Single, gen}; }
        static constexpr auto group(usize owner, usize next, u32 gen) noexcept -> Desc {
            return {State::Group, gen, next, owner};
        }
    };
    static_assert(sizeof(Desc) == 24);

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
    DirectMap direct_map_{};
    libk::InplaceVector<Arena, max_regions> arenas_{};
    libk::InplaceVector<ReservationRecord, max_regions> reservations_{};
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
    // Transfers the source chain, relabelling its descriptors in O(n).
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

enum class SlabErr : u8 {
    OutOfMemory,
    QuotaExceeded,
    GenerationExhausted,
    ResourceExhausted,
};

struct SlabQuota {
    usize nodes{4096};
    usize pages{64};
};

// The domain serializes payload access; this pool owns only storage and ids.
template <class T, bool Indexed = true, bool Trim = true> class Slab final {
    struct Slot;
    using Storage = base::slab<Slot, OwnedPage, page_size, resource::Sponsorship>;
    using PageHeader = typename Storage::page;
    struct Id {
        u64 generation{};
        bool occupied{};
    };
    struct Empty {};
    struct Slot {
        PageHeader* page{};
        Slot* next_free{};
        [[no_unique_address]] std::conditional_t<Indexed, Id, Empty> id{};
        alignas(T) byte storage[sizeof(T)]{};
        auto object() noexcept -> T* { return reinterpret_cast<T*>(storage); }
    };

  public:
    using Quota = SlabQuota;
    struct Entry {
        T* object{};
        libk::key<T> key{};
    };
    static constexpr auto quota_for(usize nodes) noexcept -> Quota {
        constexpr auto count = Storage::capacity();
        return {nodes, nodes / count + (nodes % count != 0)};
    }
    static constexpr auto slot_size() noexcept -> usize { return sizeof(Slot); }
    explicit Slab(Pmm& pmm, Quota quota = {~usize{}, ~usize{}},
                  resource::Sponsorship* sponsor = nullptr) noexcept
        : pmm_(&pmm), quota_(quota), sponsor_(sponsor) {}
    Slab(const Slab&) = delete;
    auto operator=(const Slab&) -> Slab& = delete;
    ~Slab() noexcept {
        libk_assert(storage_.live() == 0 && growing_ == 0);
        while (auto* page = storage_.take_page())
            release_page(*page);
    }
    void bind_sponsor(resource::Sponsorship& sponsor) noexcept {
        sync::Lock guard{lock_};
        libk_assert(!sponsor_ && sponsor && storage_.pages() == 0 && growing_ == 0);
        sponsor_ = &sponsor;
    }
    using Result = std::conditional_t<Indexed, Entry, T*>;
    template <class... Args> auto create(Args&&... args) noexcept -> std::expected<Result, SlabErr> {
        auto claimed = claim();
        if (!claimed) return std::unexpected(claimed.error());
        auto* slot = claimed.value();
        auto* object = std::construct_at(slot->object(), std::forward<Args>(args)...);
        if constexpr (Indexed) return Entry{object, key_of(*object)};
        else return object;
    }
    void destroy(T& object) noexcept {
        auto& slot = slot_of(object);
        if constexpr (Indexed) libk_assert(slot.id.occupied);
        std::destroy_at(&object);
        PageHeader* page{};
        {
            sync::Lock guard{lock_};
            libk_assert(storage_.live() != 0);
            if constexpr (Indexed) slot.id.occupied = false;
            // Content metadata remains reusable across allocation failures.
            page = storage_.release(slot, Trim);
        }
        if (page) release_page(*page);
    }
    auto find(libk::key<T> key) noexcept -> T*
        requires Indexed
    {
        sync::Lock guard{lock_};
        if (!key.valid()) return nullptr;
        auto* slot = storage_.find(key.slot);
        return slot && slot->id.occupied && slot->id.generation == key.generation ? slot->object() : nullptr;
    }
    auto key_of(const T& object) const noexcept -> libk::key<T>
        requires Indexed
    {
        const auto& slot = slot_of(object);
        if constexpr (Indexed) libk_assert(slot.id.occupied);
        return {reinterpret_cast<usize>(&slot), slot.id.generation};
    }
    auto live_count() const noexcept -> usize {
        sync::Lock guard{lock_};
        return storage_.live();
    }

  private:
    static auto slot_of(const T& object) noexcept -> const Slot& {
        return *reinterpret_cast<const Slot*>(reinterpret_cast<const byte*>(&object) -
                                              __builtin_offsetof(Slot, storage));
    }
    static auto slot_of(T& object) noexcept -> Slot& {
        return const_cast<Slot&>(slot_of(std::as_const(object)));
    }
    auto claim() noexcept -> std::expected<Slot*, SlabErr> {
        for (;;) {
            {
                sync::Lock guard{lock_};
                if (storage_.exhausted()) return std::unexpected(SlabErr::GenerationExhausted);
                if (storage_.live() >= quota_.nodes) return std::unexpected(SlabErr::QuotaExceeded);
                if (auto entry = storage_.claim(); entry.slot) {
                    if constexpr (Indexed) entry.slot->id = {entry.generation, true};
                    return (entry.slot);
                }
                if (storage_.pages() + growing_ >= quota_.pages)
                    return std::unexpected(SlabErr::QuotaExceeded);
                ++growing_;
            }
            auto made = make_page();
            sync::Lock guard{lock_};
            libk_assert(growing_ != 0);
            --growing_;
            if (!made) return std::unexpected(made.error());
            storage_.add(*made.value());
        }
    }
    auto make_page() noexcept -> std::expected<PageHeader*, SlabErr> {
        resource::Reservation charge;
        if (sponsor_) {
            auto reserved = sponsor_->reserve(resource::budget{.memory = page_size});
            if (!reserved) return std::unexpected(SlabErr::ResourceExhausted);
            charge = std::move(reserved).value();
        }
        auto backing = pmm_->allocate_page();
        if (!backing) return std::unexpected(SlabErr::OutOfMemory);
        auto* page = Storage::make(std::move(backing).value(), [](Slot&) noexcept {});
        if (charge) page->extra.commit(std::move(charge));
        return (page);
    }
    static void release_page(PageHeader& page) noexcept {
        libk_assert(page.live == 0);
        auto refund = page.extra.detach();
        auto backing = Storage::dispose(page);
        backing.reset();
        refund.complete();
    }

    Pmm* pmm_;
    Quota quota_;
    mutable sync::Spin lock_{};
    Storage storage_{};
    usize growing_{};
    resource::Sponsorship* sponsor_{};
};

} // namespace mm
