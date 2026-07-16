#pragma once

#include <stddef.h>
#include <stdint.h>

#include <core/types.hpp>
#include <libk/expected.hpp>
#include <libk/inplace_vector.hpp>
#include <libk/limits.hpp>
#include <libk/memory.hpp>
#include <libk/optional.hpp>
#include <libk/manual_lifetime.hpp>
#include <libk/noncopyable.hpp>
#include <libk/sync/ticket_spin_lock.hpp>
#include <mm/boot_map.hpp>
#include <mm/direct_map.hpp>

namespace kernel::mm {

enum class PageState : uint8_t {
    Reserved,
    Free,
    Allocated,
};

enum class PmmInitError : uint8_t {
    EmptyMemoryMap,
    InvalidRegion,
    OverlappingRegions,
    NoAvailableRam,
    MetadataOverflow,
    NoMetadataStorage,
};

enum class AllocError : uint8_t {
    OutOfMemory,
};

enum class QueryError : uint8_t {
    NotManaged,
};

enum class BootReclaimError : uint8_t {
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
class OwnedPageGroup;
class OwnedPageGroupExtension;

class OwnedPage {
  public:
    OwnedPage() noexcept = default;
    OwnedPage(const OwnedPage&) = delete;
    auto operator=(const OwnedPage&) -> OwnedPage& = delete;

    OwnedPage(OwnedPage&& other) noexcept;
    auto operator=(OwnedPage&& other) noexcept -> OwnedPage&;
    ~OwnedPage() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] auto page() const noexcept -> Page;
    [[nodiscard]] auto bytes() noexcept -> byte*;
    [[nodiscard]] auto bytes() const noexcept -> const byte*;
    auto reset() noexcept -> void;

  private:
    friend class Pmm;
    friend class OwnedPageGroup;

    OwnedPage(Pmm& owner, Page page, uint32_t generation) noexcept;
    auto disarm() noexcept -> void;

    libk::observer_ptr<Pmm> owner_{};
    Page page_{};
    uint32_t generation_{};
};

class BootReservation {
  public:
    BootReservation(const BootReservation&) = delete;
    auto operator=(const BootReservation&) -> BootReservation& = delete;

    BootReservation(BootReservation&& other) noexcept;
    auto operator=(BootReservation&& other) noexcept -> BootReservation&;
    ~BootReservation() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] auto range() const noexcept -> PageRange;
    auto reset() noexcept -> void;

  private:
    friend class Pmm;

    BootReservation(Pmm& owner, size_t id, PageRange range) noexcept;
    auto disarm() noexcept -> void;

    libk::observer_ptr<Pmm> owner_{};
    size_t id_{libk::numeric_limits<size_t>::max()};
    PageRange range_{};
};

class Pmm {
    class ConstructionKey {
        friend class Pmm;
        constexpr ConstructionKey() noexcept = default;
    };

  public:
    using InitializationResult = libk::Expected<void, PmmInitError>;
    using AllocateResult = libk::Expected<OwnedPage, AllocError>;
    using GroupAllocateResult = libk::Expected<Page, AllocError>;
    using QueryResult = libk::Expected<PageState, QueryError>;
    using ReclaimResult = libk::Expected<size_t, BootReclaimError>;

    [[nodiscard]] static auto initialize_in(
        libk::ManualLifetime<Pmm>& storage,
        DirectMap& direct_map,
        RegionList&& memory_map) noexcept
        -> InitializationResult;

    explicit Pmm(
        [[maybe_unused]] ConstructionKey key,
        DirectMap& direct_map) noexcept
        : direct_map_(&direct_map) {}

    Pmm(const Pmm&) = delete;
    auto operator=(const Pmm&) -> Pmm& = delete;
    Pmm(Pmm&& other) = delete;
    auto operator=(Pmm&&) -> Pmm& = delete;
    ~Pmm() noexcept;

    [[nodiscard]] auto allocate_page() noexcept -> AllocateResult;
    [[nodiscard]] auto make_page_group() noexcept -> OwnedPageGroup;
    [[nodiscard]] auto take_boot_reservation() noexcept -> libk::optional<BootReservation>;
    [[nodiscard]] auto take_boot_reservation_for(PageRange range) noexcept
        -> libk::optional<BootReservation>;
    [[nodiscard]] auto reclaim(BootReservation&& reservation) noexcept -> ReclaimResult;

    [[nodiscard]] auto contains(Page page) const noexcept -> bool;
    [[nodiscard]] auto state_of(Page page) const noexcept -> QueryResult;
    [[nodiscard]] auto free_page_count() const noexcept -> size_t;
    [[nodiscard]] auto arena_count() const noexcept -> size_t;
    [[nodiscard]] auto metadata_page_count() const noexcept -> size_t;
    [[nodiscard]] auto direct_map(this auto& self) noexcept
        -> decltype(auto) {
        return *self.direct_map_;
    }
    [[nodiscard]] auto bytes(Page page) noexcept -> byte*;
    [[nodiscard]] auto bytes(Page page) const noexcept -> const byte*;
    [[nodiscard]] auto stats() const noexcept -> PmmStats;
    [[nodiscard]] auto verify_invariants() const noexcept -> bool;

  private:
    friend class OwnedPage;
    friend class OwnedPageGroup;
    friend class OwnedPageGroupExtension;
    friend class BootReservation;

    template <typename Tag> class Index {
      public:
        constexpr Index() noexcept : value_(invalid_value_) {}
        constexpr explicit Index(size_t value) noexcept : value_(value) {}

        [[nodiscard]] constexpr auto valid() const noexcept -> bool {
            return value_ != invalid_value_;
        }

        [[nodiscard]] constexpr auto raw() const noexcept -> size_t { return value_; }

        friend constexpr auto operator==(Index lhs, Index rhs) noexcept -> bool {
            return lhs.value_ == rhs.value_;
        }

      private:
        static constexpr size_t invalid_value_ =
            libk::numeric_limits<size_t>::max();

        size_t value_;
    };

    struct FrameIndexTag;
    struct ReservationIdTag;
    struct GlobalFrameIdTag;
    struct GroupIdTag;

    using FrameIndex = Index<FrameIndexTag>;
    using ReservationId = Index<ReservationIdTag>;
    using GlobalFrameId = Index<GlobalFrameIdTag>;
    using GroupId = Index<GroupIdTag>;

    enum class FrameDescState : uint8_t {
        Reserved,
        Free,
        AllocatedIndividual,
        AllocatedGroup,
    };

    struct ReservedFrameData {
        ReservationId reservation;
        uint32_t generation;
    };

    struct FreeFrameData {
        FrameIndex next;
        uint32_t generation;
    };

    struct IndividualFrameData {
        uint32_t generation;
    };

    struct GroupFrameData {
        GlobalFrameId next;
        GroupId group;
        uint32_t generation;
    };

    union FrameDescData {
        ReservedFrameData reserved;
        FreeFrameData free;
        IndividualFrameData individual;
        GroupFrameData group;

        constexpr FrameDescData() noexcept
            : reserved{ReservationId{}, 0} {}

        constexpr explicit FrameDescData(
            ReservedFrameData value) noexcept : reserved(value) {}

        constexpr explicit FrameDescData(
            FreeFrameData value) noexcept : free(value) {}
        constexpr explicit FrameDescData(
            IndividualFrameData value) noexcept : individual(value) {}

        constexpr explicit FrameDescData(
            GroupFrameData value) noexcept : group(value) {}
    };

    struct FrameDesc {
        FrameDescState state{FrameDescState::Reserved};
        FrameDescData data{};

        [[nodiscard]] static constexpr auto reserved(
            ReservationId reservation = {},
            uint32_t generation = 0) noexcept -> FrameDesc {
            return {
                FrameDescState::Reserved,
                FrameDescData{
                    ReservedFrameData{reservation, generation}},
            };
        }

        [[nodiscard]] static constexpr auto free(
            uint32_t generation = 0,
            FrameIndex next = {}) noexcept -> FrameDesc {
            return {
                FrameDescState::Free,
                FrameDescData{FreeFrameData{next, generation}},
            };
        }

        [[nodiscard]] static constexpr auto individual(
            uint32_t generation) noexcept -> FrameDesc {
            return {
                FrameDescState::AllocatedIndividual,
                FrameDescData{IndividualFrameData{generation}},
            };
        }

        [[nodiscard]] static constexpr auto group(
            GroupId group,
            GlobalFrameId next,
            uint32_t generation) noexcept -> FrameDesc {
            return {
                FrameDescState::AllocatedGroup,
                FrameDescData{
                    GroupFrameData{next, group, generation}},
            };
        }
    };

    static_assert(sizeof(FrameDesc) == 32);

    struct Arena {
        PageRange range{};
        PageRange descriptor_storage{};
        FrameIndex free_head{};
        size_t free_count{};
    };

    enum class ReservationState : uint8_t {
        Available,
        Issued,
        Reclaimed,
    };

    struct ReservationRecord {
        PageRange range{};
        ReservationState state{ReservationState::Available};
    };


    [[nodiscard]] auto initialize(RegionList&& memory_map) noexcept
        -> InitializationResult;
    [[nodiscard]] auto descriptor_at(Arena& arena, FrameIndex index) noexcept
        -> FrameDesc&;
    [[nodiscard]] auto descriptor_at(
        const Arena& arena,
        FrameIndex index) const noexcept
        -> const FrameDesc&;
    [[nodiscard]] static auto page_at(const Arena& arena, FrameIndex index) noexcept
        -> Page;
    [[nodiscard]] static auto index_of(const Arena& arena, Page page) noexcept
        -> FrameIndex;

    [[nodiscard]] static auto public_state_of(FrameDescState state) noexcept -> PageState;
    [[nodiscard]] static auto global_frame_id_of(Page page) noexcept -> GlobalFrameId;
    [[nodiscard]] static auto page_from(GlobalFrameId id) noexcept -> Page;
    [[nodiscard]] auto next_group_id() noexcept -> GroupId;
    [[nodiscard]] auto find_arena(Page page) noexcept -> Arena*;
    [[nodiscard]] auto find_arena(Page page) const noexcept -> const Arena*;
    auto push_free(Arena& arena, FrameIndex index) noexcept -> void;
    [[nodiscard]] auto pop_free(Arena& arena) noexcept -> FrameIndex;
    [[nodiscard]] auto allocate_page_into(OwnedPageGroup& group) noexcept -> GroupAllocateResult;
    [[nodiscard]] auto detach_page(
        OwnedPageGroup& group,
        Page page) noexcept -> libk::optional<OwnedPage>;
    [[nodiscard]] auto detach_group_head(
        OwnedPageGroup& group) noexcept -> libk::optional<OwnedPage>;
    [[nodiscard]] auto attach_page(
        OwnedPageGroup& group,
        OwnedPage& page) noexcept -> bool;
    [[nodiscard]] auto group_contains(
        const OwnedPageGroup& group,
        Page page) const noexcept -> bool;
    auto release(Page page, uint32_t generation) noexcept -> void;
    auto release_group_head(OwnedPageGroup& group) noexcept -> void;
    auto release(OwnedPageGroup& group) noexcept -> void;
    auto cancel(BootReservation& reservation) noexcept -> void;
    [[nodiscard]] auto verify(const Arena& arena) const noexcept -> bool;
    [[nodiscard]] auto verify_invariants_unlocked() const noexcept -> bool;

    mutable libk::TicketSpinLock lock_{};
    libk::observer_ptr<DirectMap> direct_map_{};
    libk::InplaceVector<Arena, max_regions> arenas_{};
    libk::InplaceVector<ReservationRecord, max_regions> reservations_{};
    size_t outstanding_pages_{};
    size_t outstanding_group_pages_{};
    size_t outstanding_groups_{};
    size_t next_group_id_{};
    size_t issued_reservations_{};
};

class OwnedPageGroup {
  public:
    OwnedPageGroup() noexcept = default;
    OwnedPageGroup(const OwnedPageGroup&) = delete;
    auto operator=(const OwnedPageGroup&) -> OwnedPageGroup& = delete;

    OwnedPageGroup(OwnedPageGroup&& other) noexcept;
    auto operator=(OwnedPageGroup&& other) noexcept -> OwnedPageGroup&;
    ~OwnedPageGroup() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] auto page_count() const noexcept -> size_t;
    [[nodiscard]] auto owner() const noexcept -> Pmm&;
    [[nodiscard]] auto contains(Page page) const noexcept -> bool;
    [[nodiscard]] auto bytes(Page page) noexcept -> byte*;
    [[nodiscard]] auto bytes(Page page) const noexcept -> const byte*;

    [[nodiscard]] auto extend() noexcept -> OwnedPageGroupExtension;
    [[nodiscard]] auto try_extend(size_t page_count) noexcept -> bool;
    // Transfers one page out of the group without releasing the frame.  This
    // is the ownership primitive used by translation detach/retire batches.
    [[nodiscard]] auto detach(Page page) noexcept
        -> libk::optional<OwnedPage>;
    // Transfers the current group head to individual ownership in O(1).
    // Prepared page-table reserves can consume a batch without maintaining a
    // second list of physical page identities.
    [[nodiscard]] auto take() noexcept -> libk::optional<OwnedPage>;
    // Regroups an individually owned frame without freeing or reallocating it.
    // Retirement batches use this to keep an unbounded set of detached table
    // pages behind one RAII owner.
    [[nodiscard]] auto attach(OwnedPage&& page) noexcept -> bool;
    auto reset() noexcept -> void;

  private:
    friend class Pmm;
    friend class OwnedPageGroupExtension;


    OwnedPageGroup(
        Pmm& owner,
        Pmm::GroupId id) noexcept;

    auto disarm() noexcept -> void;

    libk::observer_ptr<Pmm> owner_{};
    Pmm::GlobalFrameId head_{};
    size_t page_count_{};
    Pmm::GroupId id_{};
    bool extension_active_{};
};

static_assert(sizeof(OwnedPageGroup) == 40);

class OwnedPageGroupExtension : private libk::noncopyable_nonmovable{
public:
    ~OwnedPageGroupExtension() noexcept;
    [[nodiscard]] auto allocate_page() noexcept -> Pmm::GroupAllocateResult;
    [[nodiscard]] auto bytes(Page page) noexcept -> byte*;
    auto commit() noexcept -> void;
private:
    friend class OwnedPageGroup;
    explicit OwnedPageGroupExtension(OwnedPageGroup& group) noexcept;
    Pmm& owner_;
    OwnedPageGroup& group_;
    Pmm::GlobalFrameId original_head_{};
    size_t original_page_count_{};
    bool committed_{};
};

static_assert(sizeof(OwnedPageGroupExtension)==40);
} // namespace kernel::mm
