#include <algorithm>
#include <expected>
#include <mm/pmm.hpp>
#include <mmu/pte.hpp>
#include <optional>

#include <base/types.hpp>
#include <libk/algorithm.hpp>
#include <libk/assert.hpp>
#include <libk/checked_arithmetic.hpp>
#include <libk/memory.hpp>
#include <sync.hpp>
#include <utility>

namespace mm {

OwnedPage::OwnedPage(Pmm& owner, Page page, uint32_t generation) noexcept
    : h_(Data{&owner, page, generation}) {}

void OwnedPage::Drop::operator()(Data& d) const noexcept { d.owner->release(d.page, d.gen); }

OwnedPage::operator bool() const noexcept { return static_cast<bool>(h_); }

auto OwnedPage::page() const noexcept -> Page {
    libk_assert(h_);
    return h_.get().page;
}

auto OwnedPage::bytes() noexcept -> byte* {
    libk_assert(h_);
    return h_.get().owner->bytes(h_.get().page);
}

auto OwnedPage::bytes() const noexcept -> const byte* {
    libk_assert(h_);
    return h_.get().owner->bytes(h_.get().page);
}

void OwnedPage::disarm() noexcept { static_cast<void>(h_.release()); }

PageGroup::PageGroup(Pmm& owner, usize id) noexcept : owner_(&owner), id_(id) {}

PageGroup::PageGroup(PageGroup&& other) noexcept
    : owner_(other.owner_), head_(other.head_), page_count_(other.page_count_), id_(other.id_) {
    other.disarm();
}

auto PageGroup::operator=(PageGroup&& other) noexcept -> PageGroup& {

    if (this != &other) {
        reset();
        owner_.reset(other.owner_.get());
        head_ = other.head_;
        page_count_ = other.page_count_;
        id_ = other.id_;
        other.disarm();
    }
    return *this;
}

PageGroup::~PageGroup() noexcept { reset(); }

PageGroup::operator bool() const noexcept { return static_cast<bool>(owner_); }

auto PageGroup::page_count() const noexcept -> size_t { return page_count_; }

auto PageGroup::owner() const noexcept -> Pmm& {
    libk_assert(owner_);
    return *owner_;
}

auto PageGroup::contains(Page page) const noexcept -> bool {
    return owner_ && owner_->group_contains(*this, page);
}

auto PageGroup::bytes(Page page) noexcept -> byte* {
    libk_assert(owner_);
    return owner_->bytes(page);
}

auto PageGroup::bytes(Page page) const noexcept -> const byte* {
    libk_assert(owner_);
    return owner_->bytes(page);
}

auto PageGroup::reset() noexcept -> void {
    if (owner_) {
        owner_->release(*this);
    }
}

auto PageGroup::disarm() noexcept -> void {
    owner_.reset();
    head_ = Pmm::Nil;
    page_count_ = 0;
    id_ = Pmm::Nil;
}

auto PageGroup::allocate() noexcept -> Pmm::GroupAllocateResult {
    libk_assert(owner_);
    return owner_->allocate_page_into(*this);
}

void PageGroup::append(PageGroup&& src) noexcept {
    libk_assert(owner_ && src.owner_.get() == owner_.get() && &src != this);
    owner_->append(*this, src);
}

auto PageGroup::grow(size_t n) noexcept -> bool {
    if (n == 0) return true;
    auto pending = owner().group();
    for (usize i = 0; i < n; ++i) {
        if (!pending.allocate()) return false;
    }
    append(std::move(pending));
    return true;
}

auto PageGroup::detach(Page page) noexcept -> std::optional<OwnedPage> {
    libk_assert(owner_);
    return owner_->detach_page(*this, page);
}

auto PageGroup::take() noexcept -> std::optional<OwnedPage> {
    libk_assert(owner_);
    return owner_->detach_group_head(*this);
}

auto PageGroup::attach(OwnedPage&& page) noexcept -> bool {
    libk_assert(owner_);
    return owner_->attach_page(*this, page);
}

void BootPages::Drop::operator()(Data& d) const noexcept { d.owner->cancel(d.id); }

auto BootPages::range() const noexcept -> Pages {
    libk_assert(h_);
    return h_.get().owner->boot_range(h_.get().id);
}

auto Pmm::initialize_in(libk::ManualLifetime<Pmm>& storage, RegionList&& memory_map,
                        DirectMap::Layout layout) noexcept -> InitializationResult {
    Pmm& memory = storage.emplace(ConstructionKey{}, std::move(memory_map), layout);
    auto result = memory.initialize();

    if (result) {
        return {};
    }
    const PmmInitError error = result.error();
    storage.reset();
    return std::unexpected(error);
}

auto Pmm::initialize() noexcept -> InitializationResult {
    if (memory_.empty()) {
        return std::unexpected(PmmInitError::EmptyMemoryMap);
    }

    for (const auto& region : memory_) {
        if (!arch::Pte::supports(region.attr)) return std::unexpected(PmmInitError::BadAttr);
        if (!region.valid()) {
            return std::unexpected(PmmInitError::InvalidRegion);
        }
    }

    libk::insertion_sort(
        memory_, [](const auto& lhs, const auto& rhs) { return lhs.range.base() < rhs.range.base(); });

    for (size_t index = 1; index < memory_.size(); ++index) {
        if (memory_[index - 1].range.intersects(memory_[index].range)) {
            return std::unexpected(PmmInitError::OverlappingRegions);
        }
    }

    bool has_available_ram = false;

    for (const auto& region : memory_) {
        if (region.kind == Region::Kind::Boot) {
            libk_assert(reservations_.try_emplace_back(ReservationRecord{.range = region.range}));
        }

        if (!region.is_ram()) {
            continue;
        }

        has_available_ram = has_available_ram || region.kind == Region::Kind::Ram;

        if (!arenas_.empty()) {
            Arena& last = arenas_[arenas_.size() - 1];
            const auto end = last.range.limit();

            if (end && *end == region.range.base()) {
                const auto pages = libk::checked_add(last.range.page_count(), region.range.page_count());
                if (!pages.has_value()) {
                    return std::unexpected(PmmInitError::MetadataOverflow);
                }
                last.range = Pages{last.range.base(), pages.value()};
                continue;
            }
        }
        libk_assert(arenas_.try_emplace_back(Arena{.range = region.range}));
    }

    if (!has_available_ram) {
        return std::unexpected(PmmInitError::NoRam);
    }

    if (!direct_map_.valid()) return std::unexpected(PmmInitError::OutsideWindow);

    for (auto& arena : arenas_) {
        const auto bytes = libk::checked_multiply(arena.range.page_count(), sizeof(Desc));
        const auto rounded =
            bytes.has_value() ? libk::checked_align_up(bytes.value(), page_size) : std::nullopt;
        if (!bytes.has_value() || !rounded.has_value()) {
            return std::unexpected(PmmInitError::MetadataOverflow);
        }

        const size_t required_pages = rounded.value() / page_size;
        for (const auto& region : memory_) {
            if (region.kind != Region::Kind::Ram) continue;
            auto first = region.range.base();
            const auto end = *region.range.limit();
            // Metadata is allocated from RAM prefixes; descriptors, not a
            // destructively shortened copy of the firmware map, record it.
            for (const auto& used : arenas_) {
                if (used.descriptor_storage.valid() && region.range.contains(used.descriptor_storage))
                    first = std::max(first, *used.descriptor_storage.limit());
            }
            if (required_pages <= end.raw() - first.raw()) {
                arena.descriptor_storage = Pages{first, required_pages};
                break;
            }
        }
        if (!arena.descriptor_storage.valid()) {
            return std::unexpected(PmmInitError::NoMetadataStorage);
        }
    }

    for (auto& arena : arenas_) {
        for (size_t offset = 0; offset < arena.range.page_count(); ++offset) {
            const usize index{offset};
            const Page page = page_at(arena, index);

            State state = State::Reserved;
            usize reservation{Nil};

            for (const auto& region : memory_) {
                if (!region.range.contains(page)) {
                    continue;
                }
                if (region.kind == Region::Kind::Ram) {
                    state = State::Free;
                } else if (region.kind == Region::Kind::Boot) {
                    for (size_t id = 0; id < reservations_.size(); ++id) {
                        if (reservations_[id].range.contains(page)) {
                            reservation = usize{id};
                            break;
                        }
                    }
                    libk_assert((reservation != Nil));
                }
                break;
            }
            for (const auto& used : arenas_) {
                if (used.descriptor_storage.contains(page)) {
                    state = State::Reserved;
                    reservation = Nil;
                    break;
                }
            }
            const Desc descriptor = state == State::Free ? Desc::free() : Desc::reserved(reservation);
            libk::construct_at(&descriptor_at(arena, index), descriptor);
        }
    }

    for (auto& arena : arenas_) {
        for (size_t offset = arena.range.page_count(); offset > 0; --offset) {
            const usize index{offset - 1};
            if (descriptor_at(arena, index).state == State::Free) {
                push_free(arena, index);
            }
        }
    }
    libk_assert(verify_invariants_unlocked());
    return {};
}

// Immutable inventory is the attribute authority, including external MMIO.
// Adjacent inventory records may cover one range only with the same CPU attribute.
auto Pmm::attr_of(Pages range) const noexcept -> std::optional<CpuAttr> {
    if (!range.valid()) return std::nullopt;
    auto at = range.base();
    std::optional<CpuAttr> attr;
    for (const auto& r : memory_) {
        if (*r.range.limit() <= at) continue;
        if (at < r.range.base() || (attr && *attr != r.attr)) return std::nullopt;
        attr = r.attr;
        if (*range.limit() <= *r.range.limit()) return attr;
        at = *r.range.limit();
    }
    return std::nullopt;
}

Pmm::~Pmm() noexcept {
    libk_assert(outstanding_pages_ == 0);
    libk_assert(outstanding_group_pages_ == 0);
    libk_assert(outstanding_groups_ == 0);
    libk_assert(issued_reservations_ == 0);
}

auto Pmm::descriptor_at(Arena& arena, usize index) noexcept -> Desc& {
    libk_assert((index != Nil) && index < arena.range.page_count());
    const auto bytes = libk::checked_multiply(index, sizeof(Desc));
    libk_assert(bytes.has_value());
    const auto address = arena.descriptor_storage.base().base().checked_add(bytes.value());
    libk_assert(address.has_value());
    auto descriptor = direct_map_.ptr<Desc>(address.value());
    libk_assert(descriptor);
    return *descriptor.value();
}

auto Pmm::descriptor_at(const Arena& arena, usize index) const noexcept -> const Desc& {
    libk_assert((index != Nil) && index < arena.range.page_count());
    const auto bytes = libk::checked_multiply(index, sizeof(Desc));
    libk_assert(bytes.has_value());
    const auto address = arena.descriptor_storage.base().base().checked_add(bytes.value());
    libk_assert(address.has_value());
    auto descriptor = direct_map_.ptr<const Desc>(address.value());
    libk_assert(descriptor);
    return *descriptor.value();
}

auto Pmm::bytes(Page page) noexcept -> byte* {
    libk_assert(direct_map_);
    auto mapped = direct_map_.ptr<byte>(page.base(), page_size);
    libk_assert(mapped);
    return mapped.value();
}

auto Pmm::bytes(Page page) const noexcept -> const byte* {
    libk_assert(direct_map_);
    auto mapped = direct_map_.ptr<const byte>(page.base(), page_size);
    libk_assert(mapped);
    return mapped.value();
}

auto Pmm::page_at(const Arena& arena, usize index) noexcept -> Page {
    libk_assert((index != Nil) && index < arena.range.page_count());
    const auto frame = arena.range.base().checked_add(index);
    libk_assert(frame);
    return Page{*frame};
}

auto Pmm::index_of(const Arena& arena, Page page) noexcept -> usize {
    libk_assert(arena.range.contains(page));
    return usize{page.raw() - arena.range.base().raw()};
}

auto Pmm::public_state_of(State state) noexcept -> PageState {
    switch (state) {
    case State::Reserved:
        return PageState::Reserved;
    case State::Free:
        return PageState::Free;
    case State::Single:
    case State::Group:
        return PageState::Allocated;
    }
    libk_assert(false);
    return PageState::Reserved;
}

auto Pmm::global_frame_id_of(Page page) noexcept -> usize {
    libk_assert(page.valid());
    return usize{static_cast<size_t>(page.raw())};
}

auto Pmm::page_from(usize id) noexcept -> Page {
    libk_assert((id != Nil));
    const Page page{
        Page{static_cast<uintptr_t>(id)},
    };
    libk_assert(page.valid());
    return page;
}

auto Pmm::next_group_id() noexcept -> usize {
    libk_assert(next_group_id_ != std::numeric_limits<size_t>::max());
    return usize{next_group_id_++};
}

auto Pmm::find_arena(Page page) noexcept -> Arena* {
    for (auto& arena : arenas_) {
        if (arena.range.contains(page)) {
            return &arena;
        }
    }
    return nullptr;
}

auto Pmm::find_arena(Page page) const noexcept -> const Arena* {
    for (const auto& arena : arenas_) {
        if (arena.range.contains(page)) {
            return &arena;
        }
    }
    return nullptr;
}

auto Pmm::push_free(Arena& arena, usize index) noexcept -> void {
    Desc& descriptor = descriptor_at(arena, index);
    libk_assert(descriptor.state == State::Free);
    descriptor.next = arena.free_head;
    arena.free_head = index;
    ++arena.free_count;
}

auto Pmm::pop_free(Arena& arena) noexcept -> usize {
    if ((arena.free_head == Nil)) {
        return Nil;
    }
    const usize index = arena.free_head;
    Desc& descriptor = descriptor_at(arena, index);
    libk_assert(descriptor.state == State::Free);
    arena.free_head = descriptor.next;
    descriptor.next = Nil;
    --arena.free_count;
    return index;
}

auto Pmm::allocate_page() noexcept -> AllocateResult {
    sync::Lock guard{lock_};
    for (auto& arena : arenas_) {
        const usize index = pop_free(arena);
        if ((index == Nil)) {
            continue;
        }
        Desc& descriptor = descriptor_at(arena, index);
        uint32_t generation = descriptor.gen + 1;
        if (generation == 0) {
            ++generation;
        }
        descriptor = Desc::individual(generation);
        ++outstanding_pages_;
        return (OwnedPage{
            *this,
            page_at(arena, index),
            generation,
        });
    }
    return std::unexpected(AllocError::NoMemory);
}

auto Pmm::group() noexcept -> PageGroup {
    sync::Lock guard{lock_};
    ++outstanding_groups_;
    return PageGroup{*this, next_group_id()};
}

auto Pmm::allocate_page_into(PageGroup& group) noexcept -> GroupAllocateResult {
    sync::Lock guard{lock_};
    libk_assert(group.owner_.get() == this);
    libk_assert((group.id_ != Nil));

    for (auto& arena : arenas_) {
        const usize index = pop_free(arena);
        if ((index == Nil)) {
            continue;
        }

        Desc& descriptor = descriptor_at(arena, index);
        const uint32_t generation = descriptor.gen;
        const Page page = page_at(arena, index);
        descriptor = Desc::group(group.id_, group.head_, generation);
        group.head_ = global_frame_id_of(page);
        ++group.page_count_;
        ++outstanding_pages_;
        ++outstanding_group_pages_;
        return (page);
    }

    return std::unexpected(AllocError::NoMemory);
}

void Pmm::append(PageGroup& dst, PageGroup& src) noexcept {
    sync::Lock guard{lock_};
    libk_assert(dst.owner_.get() == this && src.owner_.get() == this);
    usize index = src.head_;
    while (index != Nil) {
        const Page page = page_from(index);
        Arena* const arena = find_arena(page);
        libk_assert(arena != nullptr);
        Desc& d = descriptor_at(*arena, index_of(*arena, page));
        libk_assert(d.state == State::Group && d.owner == src.id_);
        d.owner = dst.id_;
        const usize next = d.next;
        if (next == Nil) d.next = dst.head_;
        index = next;
    }
    if (src.head_ != Nil) dst.head_ = src.head_;
    dst.page_count_ += src.page_count_;
    libk_assert(outstanding_groups_ != 0);
    --outstanding_groups_;
    src.disarm();
}

auto Pmm::detach_page(PageGroup& group, Page page) noexcept -> std::optional<OwnedPage> {
    sync::Lock guard{lock_};
    libk_assert(group.owner_.get() == this);
    libk_assert((group.id_ != Nil));

    usize previous{Nil};
    usize current = group.head_;
    while ((current != Nil)) {
        const Page current_page = page_from(current);
        Arena* const arena = find_arena(current_page);
        libk_assert(arena != nullptr);
        Desc& descriptor = descriptor_at(*arena, index_of(*arena, current_page));
        libk_assert(descriptor.state == State::Group);
        libk_assert(descriptor.owner == group.id_);

        if (current_page == page) {
            const usize next = descriptor.next;
            uint32_t generation = descriptor.gen + 1;
            if (generation == 0) {
                ++generation;
            }
            if ((previous == Nil)) {
                group.head_ = next;
            } else {
                const Page previous_page = page_from(previous);
                Arena* const previous_arena = find_arena(previous_page);
                libk_assert(previous_arena != nullptr);
                Desc& previous_descriptor =
                    descriptor_at(*previous_arena, index_of(*previous_arena, previous_page));
                previous_descriptor.next = next;
            }
            descriptor = Desc::individual(generation);
            --group.page_count_;
            --outstanding_group_pages_;
            return std::optional<OwnedPage>{OwnedPage{*this, page, generation}};
        }
        previous = current;
        current = descriptor.next;
    }
    return std::nullopt;
}

auto Pmm::detach_group_head(PageGroup& group) noexcept -> std::optional<OwnedPage> {
    sync::Lock guard{lock_};
    libk_assert(group.owner_.get() == this);
    libk_assert((group.id_ != Nil));
    if ((group.head_ == Nil)) {
        libk_assert(group.page_count_ == 0);
        return std::nullopt;
    }

    const Page page = page_from(group.head_);
    Arena* const arena = find_arena(page);
    libk_assert(arena != nullptr);
    Desc& descriptor = descriptor_at(*arena, index_of(*arena, page));
    libk_assert(descriptor.state == State::Group);
    libk_assert(descriptor.owner == group.id_);

    const usize next = descriptor.next;
    uint32_t generation = descriptor.gen + 1;
    if (generation == 0) {
        ++generation;
    }
    descriptor = Desc::individual(generation);
    group.head_ = next;
    libk_assert(group.page_count_ != 0);
    --group.page_count_;
    libk_assert(outstanding_group_pages_ != 0);
    --outstanding_group_pages_;
    return std::optional<OwnedPage>{OwnedPage{*this, page, generation}};
}

auto Pmm::attach_page(PageGroup& group, OwnedPage& page) noexcept -> bool {
    sync::Lock guard{lock_};
    libk_assert(group.owner_.get() == this);
    libk_assert((group.id_ != Nil));

    if (page.h_.get().owner != this || !page.h_.get().page.valid()) {
        return false;
    }
    Arena* const arena = find_arena(page.h_.get().page);
    if (arena == nullptr) {
        return false;
    }
    Desc& descriptor = descriptor_at(*arena, index_of(*arena, page.h_.get().page));
    if (descriptor.state != State::Single || descriptor.gen != page.h_.get().gen) {
        return false;
    }

    descriptor = Desc::group(group.id_, group.head_, page.h_.get().gen);
    group.head_ = global_frame_id_of(page.h_.get().page);
    ++group.page_count_;
    ++outstanding_group_pages_;
    page.disarm();
    return true;
}

auto Pmm::group_contains(const PageGroup& group, Page page) const noexcept -> bool {
    sync::Lock guard{lock_};
    libk_assert(group.owner_.get() == this);
    const auto* arena = find_arena(page);
    if (!arena) return false;
    const auto& desc = descriptor_at(*arena, index_of(*arena, page));
    // The descriptor is the ownership authority; the chain is only the group's release index.
    return desc.state == State::Group && desc.owner == group.id_;
}

auto Pmm::release(Page page, uint32_t generation) noexcept -> void {
    sync::Lock guard{lock_};
    Arena* arena = find_arena(page);
    libk_assert(arena != nullptr);
    const usize index = index_of(*arena, page);
    Desc& descriptor = descriptor_at(*arena, index);
    libk_assert(descriptor.state == State::Single);
    libk_assert(descriptor.gen == generation);
    descriptor = Desc::free(generation);
    --outstanding_pages_;
    push_free(*arena, index);
}

auto Pmm::release_group_head(PageGroup& group) noexcept -> void {
    sync::Lock guard{lock_};
    libk_assert(group.owner_.get() == this);
    libk_assert((group.id_ != Nil));
    libk_assert((group.head_ != Nil));
    libk_assert(group.page_count_ != 0);
    libk_assert(outstanding_pages_ != 0);
    libk_assert(outstanding_group_pages_ != 0);

    const Page page = page_from(group.head_);
    Arena* arena = find_arena(page);
    libk_assert(arena != nullptr);

    const usize index = index_of(*arena, page);
    Desc& descriptor = descriptor_at(*arena, index);

    libk_assert(descriptor.state == State::Group);
    libk_assert(descriptor.owner == group.id_);

    const usize next = descriptor.next;
    const uint32_t generation = descriptor.gen;

    descriptor = Desc::free(generation);
    push_free(*arena, index);
    group.head_ = next;
    --group.page_count_;
    --outstanding_pages_;
    --outstanding_group_pages_;
}

auto Pmm::release(PageGroup& group) noexcept -> void {
    libk_assert(group.owner_.get() == this);
    libk_assert((group.id_ != Nil));

    while (group.page_count_ != 0) {
        release_group_head(group);
    }

    sync::Lock guard{lock_};
    libk_assert((group.head_ == Nil));
    libk_assert(outstanding_groups_ != 0);

    --outstanding_groups_;
    group.disarm();
}

auto Pmm::take_boot() noexcept -> std::optional<BootPages> {
    sync::Lock guard{lock_};
    for (size_t id = 0; id < reservations_.size(); ++id) {
        auto& record = reservations_[id];
        if (record.state != ReservationState::Available) {
            continue;
        }
        record.state = ReservationState::Issued;
        ++issued_reservations_;
        BootPages reservation{*this, id};
        return std::optional<BootPages>{std::move(reservation)};
    }
    return std::nullopt;
}

auto Pmm::take_boot(Pages range) noexcept -> std::optional<BootPages> {
    sync::Lock guard{lock_};
    if (!range.valid()) {
        return std::nullopt;
    }
    for (size_t id = 0; id < reservations_.size(); ++id) {
        auto& record = reservations_[id];
        if (record.state != ReservationState::Available || record.range.base() != range.base() ||
            record.range.page_count() != range.page_count()) {
            continue;
        }
        record.state = ReservationState::Issued;
        ++issued_reservations_;
        BootPages reservation{*this, id};
        return std::optional<BootPages>{std::move(reservation)};
    }
    return std::nullopt;
}

auto Pmm::reclaim(BootPages&& reservation) noexcept -> ReclaimResult {
    sync::Lock guard{lock_};
    if (reservation.h_.get().owner != this) {
        return std::unexpected(BootErr::WrongOwner);
    }
    if (reservation.h_.get().id >= reservations_.size() ||
        reservations_[reservation.h_.get().id].state != ReservationState::Issued) {
        return std::unexpected(BootErr::InvalidReservation);
    }

    const usize id{reservation.h_.get().id};
    for (const Page page : reservations_[reservation.h_.get().id].range) {
        Arena* arena = find_arena(page);
        if (arena == nullptr) {
            return std::unexpected(BootErr::InvalidReservation);
        }
        const Desc& descriptor = descriptor_at(*arena, index_of(*arena, page));
        if (descriptor.state != State::Reserved || descriptor.owner != id) {
            return std::unexpected(BootErr::InvalidReservation);
        }
    }

    for (const Page page : reservations_[reservation.h_.get().id].range) {
        Arena& arena = *find_arena(page);
        const usize index = index_of(arena, page);
        Desc& descriptor = descriptor_at(arena, index);
        descriptor = Desc::free(descriptor.gen);
        push_free(arena, index);
    }

    reservations_[reservation.h_.get().id].state = ReservationState::Consumed;
    --issued_reservations_;
    const size_t reclaimed = reservations_[reservation.h_.get().id].range.page_count();
    (void)reservation.h_.release();
    libk_assert(verify_invariants_unlocked());
    return (reclaimed);
}

auto Pmm::adopt(BootPages&& reservation) noexcept -> AdoptResult {
    sync::Lock guard{lock_};
    if (reservation.h_.get().owner != this) {
        return std::unexpected(BootErr::WrongOwner);
    }
    if (reservation.h_.get().id >= reservations_.size() ||
        reservations_[reservation.h_.get().id].state != ReservationState::Issued) {
        return std::unexpected(BootErr::InvalidReservation);
    }

    const usize reservation_id{reservation.h_.get().id};
    for (const Page page : reservations_[reservation.h_.get().id].range) {
        Arena* const arena = find_arena(page);
        if (arena == nullptr) {
            return std::unexpected(BootErr::InvalidReservation);
        }
        const Desc& descriptor = descriptor_at(*arena, index_of(*arena, page));
        if (descriptor.state != State::Reserved || descriptor.owner != reservation_id) {
            return std::unexpected(BootErr::InvalidReservation);
        }
    }

    const usize group_id = next_group_id();
    PageGroup pages{*this, group_id};
    ++outstanding_groups_;
    for (const Page page : reservations_[reservation.h_.get().id].range) {
        Arena& arena = *find_arena(page);
        Desc& descriptor = descriptor_at(arena, index_of(arena, page));
        const uint32_t generation = descriptor.gen;
        descriptor = Desc::group(group_id, pages.head_, generation);
        pages.head_ = global_frame_id_of(page);
        ++pages.page_count_;
        ++outstanding_pages_;
        ++outstanding_group_pages_;
    }

    reservations_[reservation.h_.get().id].state = ReservationState::Consumed;
    --issued_reservations_;
    (void)reservation.h_.release();
    libk_assert(verify_invariants_unlocked());
    return pages;
}

auto Pmm::boot_range(usize id) const noexcept -> Pages {
    sync::Lock guard{lock_};
    libk_assert(id < reservations_.size() && reservations_[id].state == ReservationState::Issued);
    return reservations_[id].range;
}

void Pmm::cancel(usize id) noexcept {
    sync::Lock guard{lock_};
    libk_assert(id < reservations_.size());
    auto& r = reservations_[id];
    libk_assert(r.state == ReservationState::Issued);
    r.state = ReservationState::Available;
    --issued_reservations_;
}

auto Pmm::contains(Page page) const noexcept -> bool {
    sync::Lock guard{lock_};
    return find_arena(page) != nullptr;
}

auto Pmm::state_of(Page page) const noexcept -> QueryResult {
    sync::Lock guard{lock_};
    const Arena* arena = find_arena(page);
    if (arena == nullptr) {
        return std::unexpected(QueryError::NotManaged);
    }
    return (public_state_of(descriptor_at(*arena, index_of(*arena, page)).state));
}

auto Pmm::page_count() const noexcept -> size_t {
    sync::Lock guard{lock_};
    size_t count = 0;
    for (const auto& arena : arenas_) {
        count += arena.range.page_count();
    }
    return count;
}

auto Pmm::free_page_count() const noexcept -> size_t {
    sync::Lock guard{lock_};
    size_t count = 0;
    for (const auto& arena : arenas_) {
        count += arena.free_count;
    }
    return count;
}

auto Pmm::arena_count() const noexcept -> size_t {
    sync::Lock guard{lock_};
    return arenas_.size();
}

auto Pmm::metadata_page_count() const noexcept -> size_t {
    sync::Lock guard{lock_};
    size_t count = 0;
    for (const auto& arena : arenas_) {
        count += arena.descriptor_storage.page_count();
    }
    return count;
}

auto Pmm::stats() const noexcept -> PmmStats {
    sync::Lock guard{lock_};
    PmmStats result{
        .arena_count = arenas_.size(),
        .boot_reservations = reservations_.size(),
    };
    for (const auto& arena : arenas_) {
        result.metadata_pages += arena.descriptor_storage.page_count();
        for (size_t offset = 0; offset < arena.range.page_count(); ++offset) {
            switch (descriptor_at(arena, usize{offset}).state) {
            case State::Reserved:
                ++result.reserved_pages;
                break;
            case State::Free:
                ++result.free_pages;
                break;
            case State::Single:
            case State::Group:
                ++result.allocated_pages;
                break;
            }
        }
    }
    size_t indexed_free{};
    for (const auto& arena : arenas_) {
        indexed_free += arena.free_count;
    }
    libk_assert(result.free_pages == indexed_free);
    libk_assert(result.allocated_pages == outstanding_pages_);
    return result;
}

auto Pmm::verify(const Arena& arena) const noexcept -> bool {
    size_t indexed = 0;
    usize current = arena.free_head;
    while ((current != Nil)) {
        if (current >= arena.range.page_count() || indexed >= arena.range.page_count()) {
            return false;
        }
        const Desc& descriptor = descriptor_at(arena, current);
        if (descriptor.state != State::Free) {
            return false;
        }
        current = descriptor.next;
        ++indexed;
    }

    size_t free = 0;
    for (size_t offset = 0; offset < arena.range.page_count(); ++offset) {
        const Desc& descriptor = descriptor_at(arena, usize{offset});
        if (descriptor.state == State::Free) {
            ++free;
        }
    }
    return indexed == free && indexed == arena.free_count;
}

auto Pmm::verify_invariants() const noexcept -> bool {
    sync::Lock guard{lock_};
    return verify_invariants_unlocked();
}

auto Pmm::verify_invariants_unlocked() const noexcept -> bool {
    size_t allocated = 0;
    size_t grouped = 0;
    size_t issued = 0;

    for (size_t index = 0; index < arenas_.size(); ++index) {
        const Arena& arena = arenas_[index];
        if (!arena.range.valid() || !arena.descriptor_storage.valid() || !verify(arena)) {
            return false;
        }

        bool storage_is_managed = false;
        for (const auto& owner : arenas_) {
            storage_is_managed = storage_is_managed || owner.range.contains(arena.descriptor_storage);
        }
        if (!storage_is_managed) {
            return false;
        }
        for (const Page page : arena.descriptor_storage) {
            const Arena* owner = find_arena(page);
            if (owner == nullptr || descriptor_at(*owner, index_of(*owner, page)).state != State::Reserved) {
                return false;
            }
        }

        for (size_t offset = 0; offset < arena.range.page_count(); ++offset) {
            const Desc& descriptor = descriptor_at(arena, usize{offset});

            if (descriptor.state == State::Single) {
                if (descriptor.gen == 0) {
                    return false;
                }
                ++allocated;
            } else if (descriptor.state == State::Group) {
                if ((descriptor.owner == Nil)) {
                    return false;
                }
                if ((descriptor.next != Nil)) {
                    const Page next_page = page_from(descriptor.next);
                    const Arena* next_arena = find_arena(next_page);
                    if (next_arena == nullptr) {
                        return false;
                    }
                    const Desc& next = descriptor_at(*next_arena, index_of(*next_arena, next_page));
                    if (next.state != State::Group || next.owner != descriptor.owner) {
                        return false;
                    }
                }
                ++allocated;
                ++grouped;
            } else if (descriptor.state == State::Reserved && (descriptor.owner != Nil)) {
                const usize reservation = descriptor.owner;
                if (reservation >= reservations_.size() ||
                    reservations_[reservation].state == ReservationState::Consumed) {
                    return false;
                }
            }
        }

        for (size_t other = index + 1; other < arenas_.size(); ++other) {
            if (arena.range.intersects(arenas_[other].range) ||
                arena.descriptor_storage.intersects(arenas_[other].descriptor_storage)) {
                return false;
            }
        }
    }

    for (size_t id = 0; id < reservations_.size(); ++id) {
        const ReservationRecord& reservation = reservations_[id];
        if (!reservation.range.valid()) {
            return false;
        }
        if (reservation.state == ReservationState::Issued) {
            ++issued;
        }
        if (reservation.state == ReservationState::Consumed) {
            continue;
        }
        for (const Page page : reservation.range) {
            const Arena* arena = find_arena(page);
            if (arena == nullptr) {
                return false;
            }
            const Desc& descriptor = descriptor_at(*arena, index_of(*arena, page));
            if (descriptor.state != State::Reserved || descriptor.owner != usize{id}) {
                return false;
            }
        }
    }

    return allocated == outstanding_pages_ && grouped == outstanding_group_pages_ &&
           issued == issued_reservations_;
}

} // namespace mm
