#include <mm/pmm.hpp>

#include <core/debug.hpp>
#include <libk/algorithm.hpp>
#include <libk/checked_arithmetic.hpp>
#include <libk/memory.hpp>
#include <libk/utility.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel::mm {

OwnedPage::OwnedPage(Pmm& owner, Page page, uint32_t generation) noexcept
    : owner_(&owner), page_(page), generation_(generation) {
}

OwnedPage::OwnedPage(OwnedPage&& other) noexcept
    : owner_(other.owner_), page_(other.page_), generation_(other.generation_) {
    other.disarm();
}

auto OwnedPage::operator=(OwnedPage&& other) noexcept -> OwnedPage& {
    if (this != &other) {
        reset();
        owner_.reset(other.owner_.get());
        page_ = other.page_;
        generation_ = other.generation_;
        other.disarm();
    }
    return *this;
}

OwnedPage::~OwnedPage() noexcept {
    reset();
}

OwnedPage::operator bool() const noexcept {
    return static_cast<bool>(owner_);
}

auto OwnedPage::page() const noexcept -> Page {
    KASSERT(owner_);
    return page_;
}

auto OwnedPage::bytes() noexcept -> byte* {
    KASSERT(owner_);
    return owner_->bytes(page_);
}

auto OwnedPage::bytes() const noexcept -> const byte* {
    KASSERT(owner_);
    return owner_->bytes(page_);
}

auto OwnedPage::reset() noexcept -> void {
    if (owner_) {
        owner_->release(page_, generation_);
        disarm();
    }
}

auto OwnedPage::disarm() noexcept -> void {
    owner_.reset();
    page_ = Page{};
    generation_ = 0;
}



OwnedPageGroup::OwnedPageGroup(
    Pmm& owner,
    Pmm::GroupId id) noexcept
    : owner_(&owner), id_(id) {
}

OwnedPageGroup::OwnedPageGroup(OwnedPageGroup&& other) noexcept
    : owner_(other.owner_),
      head_(other.head_),
      page_count_(other.page_count_),
      id_(other.id_) {
    KASSERT(!other.extension_active_);
    other.disarm();
}

auto OwnedPageGroup::operator=(OwnedPageGroup&& other) noexcept
    -> OwnedPageGroup& {

    KASSERT(!extension_active_);
    KASSERT(!other.extension_active_);

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

OwnedPageGroup::~OwnedPageGroup() noexcept {
    reset();
}

OwnedPageGroup::operator bool() const noexcept {
    return static_cast<bool>(owner_);
}

auto OwnedPageGroup::page_count() const noexcept -> size_t {
    return page_count_;
}

auto OwnedPageGroup::owner() const noexcept -> Pmm& {
    KASSERT(owner_);
    return *owner_;
}

auto OwnedPageGroup::contains(Page page) const noexcept -> bool {
    return owner_ && owner_->group_contains(*this, page);
}

auto OwnedPageGroup::bytes(Page page) noexcept -> byte* {
    KASSERT(owner_);
    return owner_->bytes(page);
}

auto OwnedPageGroup::bytes(Page page) const noexcept -> const byte* {
    KASSERT(owner_);
    return owner_->bytes(page);
}

auto OwnedPageGroup::reset() noexcept -> void {
    KASSERT(!extension_active_);
    if (owner_) {
        owner_->release(*this);
    }
}

auto OwnedPageGroup::disarm() noexcept -> void {
    owner_.reset();
    head_ = Pmm::GlobalFrameId{};
    page_count_ = 0;
    id_ = Pmm::GroupId{};
    extension_active_ = false;
}

BootReservation::BootReservation(
    Pmm& owner,
    size_t id,
    PageRange range) noexcept
    : owner_(&owner), id_(id), range_(range) {
}

auto OwnedPageGroup::extend() noexcept -> OwnedPageGroupExtension{
    KASSERT(owner_);
    KASSERT(id_.valid());
    KASSERT(!extension_active_);

    return OwnedPageGroupExtension{*this};
}

auto OwnedPageGroup::try_extend(size_t page_count) noexcept -> bool {
    if (page_count == 0) {
        return true;
    }
    auto extension = extend();
    for (size_t index = 0; index < page_count; ++index) {
        if (!extension.allocate_page()) {
            return false;
        }
    }
    extension.commit();
    return true;
}

auto OwnedPageGroup::detach(Page page) noexcept
    -> libk::optional<OwnedPage> {
    KASSERT(owner_);
    KASSERT(!extension_active_);
    return owner_->detach_page(*this, page);
}

auto OwnedPageGroup::take() noexcept -> libk::optional<OwnedPage> {
    KASSERT(owner_);
    KASSERT(!extension_active_);
    return owner_->detach_group_head(*this);
}

auto OwnedPageGroup::attach(OwnedPage&& page) noexcept -> bool {
    KASSERT(owner_);
    KASSERT(!extension_active_);
    return owner_->attach_page(*this, page);
}

OwnedPageGroupExtension::OwnedPageGroupExtension(OwnedPageGroup& group) noexcept :
    owner_(*group.owner_),
    group_(group),
    original_head_(group.head_),
    original_page_count_(group.page_count_){
        group_.extension_active_ = true;
}

OwnedPageGroupExtension::~OwnedPageGroupExtension() noexcept {
    KASSERT(group_.owner_.get() == &owner_);

    if(committed_){
        KASSERT(!group_.extension_active_);
        return;
    }
    KASSERT(group_.extension_active_);
    KASSERT(group_.page_count_ >= original_page_count_);

    while(group_.page_count_ > original_page_count_){
        owner_.release_group_head(group_);
    }

    KASSERT(group_.head_ == original_head_);
    group_.extension_active_ = false;
}

auto OwnedPageGroupExtension::allocate_page() noexcept -> Pmm::GroupAllocateResult{
    KASSERT(group_.extension_active_);
    KASSERT(!committed_);
    KASSERT(group_.owner_.get() == &owner_);
    return owner_.allocate_page_into(group_);
}

auto OwnedPageGroupExtension::bytes(Page page) noexcept -> byte* {
    KASSERT(group_.extension_active_);
    return owner_.bytes(page);
}

auto OwnedPageGroupExtension::commit() noexcept -> void {
    KASSERT(group_.extension_active_);
    KASSERT(!committed_);
    KASSERT(group_.owner_.get() == & owner_);
    committed_ = true;
    group_.extension_active_ = false;
}


BootReservation::BootReservation(BootReservation&& other) noexcept
    : owner_(other.owner_), id_(other.id_), range_(other.range_) {
    other.disarm();
}

auto BootReservation::operator=(BootReservation&& other) noexcept -> BootReservation& {
    if (this != &other) {
        reset();
        owner_.reset(other.owner_.get());
        id_ = other.id_;
        range_ = other.range_;
        other.disarm();
    }
    return *this;
}

BootReservation::~BootReservation() noexcept {
    reset();
}

BootReservation::operator bool() const noexcept {
    return static_cast<bool>(owner_);
}

auto BootReservation::range() const noexcept -> PageRange {
    KASSERT(owner_);
    return range_;
}

auto BootReservation::reset() noexcept -> void {
    if (owner_) {
        owner_->cancel(*this);
    }
}

auto BootReservation::disarm() noexcept -> void {
    owner_.reset();
    id_ = libk::numeric_limits<size_t>::max();
    range_ = PageRange{};
}

auto Pmm::initialize_in(
    libk::ManualLifetime<Pmm>& storage,
    DirectMap& direct_map,
    RegionList&& memory_map) noexcept -> InitializationResult {
    Pmm& memory = storage.emplace(ConstructionKey{}, direct_map);
    auto result = memory.initialize(libk::move(memory_map));

    if (result) {
        return libk::expected();
    }
    const PmmInitError error = result.error();
    storage.reset();
    return libk::unexpected(error);
}

auto Pmm::initialize(
    RegionList&& memory_map) noexcept -> InitializationResult {
    if (memory_map.empty()) {
        return libk::unexpected(
            PmmInitError::EmptyMemoryMap);
    }

    for (const auto& region : memory_map) {
        if (!region.valid()) {
            return libk::unexpected(
                PmmInitError::InvalidRegion);
        }
    }

    libk::insertion_sort(
        memory_map,
        [](const auto& lhs, const auto& rhs){
            return lhs.range.first() < rhs.range.first();
        });

    for (size_t index = 1; index < memory_map.size(); ++index) {
        if (memory_map[index - 1].range.intersects(
                memory_map[index].range)) {
            return libk::unexpected(
                PmmInitError::OverlappingRegions);
        }
    }

    bool has_available_ram = false;

    for (const auto& region : memory_map) {
        if (region.kind == RegionKind::ReclaimableBootData) {
            KASSERT(reservations_.try_emplace_back(
                ReservationRecord{.range = region.range}));
        }

        if (!region.is_ram()) {
            continue;
        }

        has_available_ram =
            has_available_ram ||
            region.kind == RegionKind::AvailableRam;

        if (!arenas_.empty()) {
            Arena& last = arenas_[arenas_.size() - 1];
            const auto end = last.range.end_frame();

            if (end && *end == region.range.first().frame()) {
                const auto pages = libk::checked_add(
                    last.range.page_count(),
                    region.range.page_count());
                if (!pages.has_value()) {
                    return libk::unexpected(
                        PmmInitError::MetadataOverflow);
                }
                last.range =
                    PageRange{last.range.first(), pages.value()};
                continue;
            }
        }
        KASSERT(arenas_.try_emplace_back(
            Arena{.range = region.range}));
    }

    if (!has_available_ram) {
        return libk::unexpected(
            PmmInitError::NoAvailableRam);
    }

    for (auto& arena : arenas_) {
        const auto bytes = libk::checked_multiply(
            arena.range.page_count(),
            sizeof(FrameDesc));
        const auto rounded =
            bytes.has_value()
                ? libk::checked_align_up(bytes.value(), page_size)
                : libk::nullopt;
        if (!bytes.has_value() || !rounded.has_value()) {
            return libk::unexpected(
                PmmInitError::MetadataOverflow);
        }

        const size_t required_pages = rounded.value() / page_size;
        for (auto& region : memory_map) {
            if (region.kind != RegionKind::AvailableRam
                || region.range.page_count() < required_pages) {
                continue;
            }
            arena.descriptor_storage = PageRange{
                region.range.first(),
                required_pages,
            };

            const size_t remaining_pages =
                region.range.page_count() - required_pages;

            if (remaining_pages == 0) {
                region.range = PageRange{};
            } else {
                const auto next =
                    region.range.first().frame().checked_add(
                        required_pages);

                KASSERT(next);

                region.range = PageRange{
                    Page{*next},
                    remaining_pages,
                };
            }
            break;
        }
        if (!arena.descriptor_storage.valid()) {
            return libk::unexpected(
                PmmInitError::NoMetadataStorage);
        }
    }

    for (auto& arena : arenas_) {
        for (size_t offset = 0; offset < arena.range.page_count(); ++offset) {
            const FrameIndex index{offset};
            const Page page = page_at(arena, index);

            FrameDescState state = FrameDescState::Reserved;
            ReservationId reservation{};

            for (const auto& region : memory_map) {
                if (!region.range.contains(page)) {
                    continue;
                }
                if (region.kind == RegionKind::AvailableRam) {
                    state = FrameDescState::Free;
                } else if (
                    region.kind
                    == RegionKind::ReclaimableBootData) {
                    for (size_t id = 0; id < reservations_.size(); ++id) {
                        if (reservations_[id].range.contains(page)) {
                            reservation = ReservationId{id};
                            break;
                        }
                    }
                    KASSERT(reservation.valid());
                }
                break;
            }
            const FrameDesc descriptor =
                state == FrameDescState::Free
                    ? FrameDesc::free()
                    : FrameDesc::reserved(reservation);
            libk::construct_at(&descriptor_at(arena, index), descriptor);
        }
    }

    for (auto& arena : arenas_) {
        for (size_t offset = arena.range.page_count(); offset > 0; --offset) {
            const FrameIndex index{offset - 1};
            if (descriptor_at(arena, index).state
                == FrameDescState::Free) {
                push_free(arena, index);
            }
        }
    }
    KASSERT(verify_invariants_unlocked());
    return libk::expected();
}

Pmm::~Pmm() noexcept {
    KASSERT(outstanding_pages_ == 0);
    KASSERT(outstanding_group_pages_ == 0);
    KASSERT(outstanding_groups_ == 0);
    KASSERT(issued_reservations_ == 0);
}

auto Pmm::descriptor_at(Arena& arena, FrameIndex index) noexcept
    -> FrameDesc& {
    KASSERT(index.valid() && index.raw() < arena.range.page_count());
    const auto bytes = libk::checked_multiply(index.raw(), sizeof(FrameDesc));
    KASSERT(bytes.has_value());
    const auto address =
        arena.descriptor_storage.first().base().checked_add(bytes.value());
    KASSERT(address.has_value());
    auto descriptor = direct_map_->ptr<FrameDesc>(address.value());
    KASSERT(descriptor);
    return *descriptor.value();
}

auto Pmm::descriptor_at(const Arena& arena, FrameIndex index) const noexcept
    -> const FrameDesc& {
    KASSERT(index.valid() && index.raw() < arena.range.page_count());
    const auto bytes = libk::checked_multiply(index.raw(), sizeof(FrameDesc));
    KASSERT(bytes.has_value());
    const auto address =
        arena.descriptor_storage.first().base().checked_add(bytes.value());
    KASSERT(address.has_value());
    auto descriptor = direct_map_->ptr<const FrameDesc>(address.value());
    KASSERT(descriptor);
    return *descriptor.value();
}

auto Pmm::bytes(Page page) noexcept -> byte* {
    KASSERT(direct_map_);
    auto mapped = direct_map_->ptr<byte>(page.base(), page_size);
    KASSERT(mapped);
    return mapped.value();
}

auto Pmm::bytes(Page page) const noexcept -> const byte* {
    KASSERT(direct_map_);
    auto mapped = direct_map_->ptr<const byte>(page.base(), page_size);
    KASSERT(mapped);
    return mapped.value();
}

auto Pmm::page_at(const Arena& arena, FrameIndex index) noexcept
    -> Page {
    KASSERT(index.valid() && index.raw() < arena.range.page_count());
    const auto frame = arena.range.first().frame().checked_add(index.raw());
    KASSERT(frame);
    return Page{*frame};
}

auto Pmm::index_of(const Arena& arena, Page page) noexcept
    -> FrameIndex {
    KASSERT(arena.range.contains(page));
    return FrameIndex{page.frame().raw() - arena.range.first().frame().raw()};
}

auto Pmm::public_state_of(FrameDescState state) noexcept
    -> PageState {
    switch (state) {
    case FrameDescState::Reserved:
        return PageState::Reserved;
    case FrameDescState::Free:
        return PageState::Free;
    case FrameDescState::AllocatedIndividual:
    case FrameDescState::AllocatedGroup:
        return PageState::Allocated;
    }
    KASSERT(false);
    return PageState::Reserved;
}

auto Pmm::global_frame_id_of(Page page) noexcept
    -> GlobalFrameId {
    KASSERT(page.valid());
    return GlobalFrameId{static_cast<size_t>(page.frame().raw())};
}

auto Pmm::page_from(GlobalFrameId id) noexcept -> Page {
    KASSERT(id.valid());
    const Page page{
        Pfn{static_cast<uintptr_t>(id.raw())},
    };
    KASSERT(page.valid());
    return page;
}

auto Pmm::next_group_id() noexcept -> GroupId {
    KASSERT(next_group_id_ != libk::numeric_limits<size_t>::max());
    return GroupId{next_group_id_++};
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

auto Pmm::push_free(Arena& arena, FrameIndex index) noexcept -> void {
    FrameDesc& descriptor = descriptor_at(arena, index);
    KASSERT(descriptor.state == FrameDescState::Free);
    descriptor.data.free.next = arena.free_head;
    arena.free_head = index;
    ++arena.free_count;
}

auto Pmm::pop_free(Arena& arena) noexcept -> FrameIndex {
    if (!arena.free_head.valid()) {
        return FrameIndex{};
    }
    const FrameIndex index = arena.free_head;
    FrameDesc& descriptor = descriptor_at(arena, index);
    KASSERT(descriptor.state == FrameDescState::Free);
    arena.free_head = descriptor.data.free.next;
    descriptor.data.free.next = FrameIndex{};
    --arena.free_count;
    return index;
}

auto Pmm::allocate_page() noexcept -> AllocateResult {
    kernel::sync::IrqLockGuard guard{lock_};
    for (auto& arena : arenas_) {
        const FrameIndex index = pop_free(arena);
        if (!index.valid()) {
            continue;
        }
        FrameDesc& descriptor = descriptor_at(arena, index);
        uint32_t generation = descriptor.data.free.generation + 1;
        if (generation == 0) {
            ++generation;
        }
        descriptor = FrameDesc::individual(generation);
        ++outstanding_pages_;
        return libk::expected(OwnedPage{
            *this,
            page_at(arena, index),
            generation,
        });
    }
    return libk::unexpected(AllocError::OutOfMemory);
}

auto Pmm::make_page_group() noexcept -> OwnedPageGroup {
    kernel::sync::IrqLockGuard guard{lock_};
    ++outstanding_groups_;
    return OwnedPageGroup{*this, next_group_id()};
}

auto Pmm::allocate_page_into(OwnedPageGroup& group) noexcept
    -> GroupAllocateResult {
    kernel::sync::IrqLockGuard guard{lock_};
    KASSERT(group.owner_.get() == this);
    KASSERT(group.id_.valid());
    KASSERT(group.extension_active_);

    for (auto& arena : arenas_) {
        const FrameIndex index = pop_free(arena);
        if (!index.valid()) {
            continue;
        }

        FrameDesc& descriptor = descriptor_at(arena, index);
        const uint32_t generation = descriptor.data.free.generation;
        const Page page = page_at(arena, index);
        descriptor = FrameDesc::group(
            group.id_, group.head_, generation);
        group.head_ = global_frame_id_of(page);
        ++group.page_count_;
        ++outstanding_pages_;
        ++outstanding_group_pages_;
        return libk::expected(page);
    }

    return libk::unexpected(AllocError::OutOfMemory);
}

auto Pmm::detach_page(
    OwnedPageGroup& group,
    Page page) noexcept -> libk::optional<OwnedPage> {
    kernel::sync::IrqLockGuard guard{lock_};
    KASSERT(group.owner_.get() == this);
    KASSERT(group.id_.valid());
    KASSERT(!group.extension_active_);

    GlobalFrameId previous{};
    GlobalFrameId current = group.head_;
    while (current.valid()) {
        const Page current_page = page_from(current);
        Arena* const arena = find_arena(current_page);
        KASSERT(arena != nullptr);
        FrameDesc& descriptor = descriptor_at(
            *arena, index_of(*arena, current_page));
        KASSERT(descriptor.state == FrameDescState::AllocatedGroup);
        KASSERT(descriptor.data.group.group == group.id_);

        if (current_page == page) {
            const GlobalFrameId next = descriptor.data.group.next;
            uint32_t generation = descriptor.data.group.generation + 1;
            if (generation == 0) {
                ++generation;
            }
            if (!previous.valid()) {
                group.head_ = next;
            } else {
                const Page previous_page = page_from(previous);
                Arena* const previous_arena = find_arena(previous_page);
                KASSERT(previous_arena != nullptr);
                FrameDesc& previous_descriptor = descriptor_at(
                    *previous_arena,
                    index_of(*previous_arena, previous_page));
                previous_descriptor.data.group.next = next;
            }
            descriptor = FrameDesc::individual(generation);
            --group.page_count_;
            --outstanding_group_pages_;
            return libk::optional<OwnedPage>{
                OwnedPage{*this, page, generation}};
        }
        previous = current;
        current = descriptor.data.group.next;
    }
    return libk::nullopt;
}

auto Pmm::detach_group_head(OwnedPageGroup& group) noexcept
    -> libk::optional<OwnedPage> {
    kernel::sync::IrqLockGuard guard{lock_};
    KASSERT(group.owner_.get() == this);
    KASSERT(group.id_.valid());
    KASSERT(!group.extension_active_);
    if (!group.head_.valid()) {
        KASSERT(group.page_count_ == 0);
        return libk::nullopt;
    }

    const Page page = page_from(group.head_);
    Arena* const arena = find_arena(page);
    KASSERT(arena != nullptr);
    FrameDesc& descriptor = descriptor_at(
        *arena, index_of(*arena, page));
    KASSERT(descriptor.state == FrameDescState::AllocatedGroup);
    KASSERT(descriptor.data.group.group == group.id_);

    const GlobalFrameId next = descriptor.data.group.next;
    uint32_t generation = descriptor.data.group.generation + 1;
    if (generation == 0) {
        ++generation;
    }
    descriptor = FrameDesc::individual(generation);
    group.head_ = next;
    KASSERT(group.page_count_ != 0);
    --group.page_count_;
    KASSERT(outstanding_group_pages_ != 0);
    --outstanding_group_pages_;
    return libk::optional<OwnedPage>{
        OwnedPage{*this, page, generation}};
}

auto Pmm::attach_page(
    OwnedPageGroup& group,
    OwnedPage& page) noexcept -> bool {
    kernel::sync::IrqLockGuard guard{lock_};
    KASSERT(group.owner_.get() == this);
    KASSERT(group.id_.valid());
    KASSERT(!group.extension_active_);

    if (page.owner_.get() != this || !page.page_.valid()) {
        return false;
    }
    Arena* const arena = find_arena(page.page_);
    if (arena == nullptr) {
        return false;
    }
    FrameDesc& descriptor = descriptor_at(
        *arena, index_of(*arena, page.page_));
    if (descriptor.state != FrameDescState::AllocatedIndividual
        || descriptor.data.individual.generation != page.generation_) {
        return false;
    }

    descriptor = FrameDesc::group(
        group.id_, group.head_, page.generation_);
    group.head_ = global_frame_id_of(page.page_);
    ++group.page_count_;
    ++outstanding_group_pages_;
    page.disarm();
    return true;
}

auto Pmm::group_contains(
    const OwnedPageGroup& group,
    Page page) const noexcept -> bool {
    kernel::sync::IrqLockGuard guard{lock_};
    KASSERT(group.owner_.get() == this);
    GlobalFrameId current = group.head_;
    while (current.valid()) {
        const Page candidate = page_from(current);
        const Arena* const arena = find_arena(candidate);
        KASSERT(arena != nullptr);
        const FrameDesc& descriptor = descriptor_at(
            *arena, index_of(*arena, candidate));
        KASSERT(descriptor.state == FrameDescState::AllocatedGroup);
        KASSERT(descriptor.data.group.group == group.id_);
        if (candidate == page) {
            return true;
        }
        current = descriptor.data.group.next;
    }
    return false;
}

auto Pmm::release(Page page, uint32_t generation) noexcept -> void {
    kernel::sync::IrqLockGuard guard{lock_};
    Arena* arena = find_arena(page);
    KASSERT(arena != nullptr);
    const FrameIndex index = index_of(*arena, page);
    FrameDesc& descriptor = descriptor_at(*arena, index);
    KASSERT(descriptor.state == FrameDescState::AllocatedIndividual);
    KASSERT(descriptor.data.individual.generation == generation);
    descriptor = FrameDesc::free(generation);
    --outstanding_pages_;
    push_free(*arena, index);
}

auto Pmm::release_group_head(OwnedPageGroup& group) noexcept -> void{
    kernel::sync::IrqLockGuard guard{lock_};
    KASSERT(group.owner_.get() == this);
    KASSERT(group.id_.valid());
    KASSERT(group.head_.valid());
    KASSERT(group.page_count_ !=0 );
    KASSERT(outstanding_pages_ != 0);
    KASSERT(outstanding_group_pages_ != 0);

    const Page page = page_from(group.head_);
    Arena* arena = find_arena(page);
    KASSERT(arena != nullptr);

    const FrameIndex index = index_of(*arena, page);
    FrameDesc& descriptor = descriptor_at(*arena, index);

    KASSERT(descriptor.state == FrameDescState::AllocatedGroup);
    KASSERT(descriptor.data.group.group == group.id_);

    const GlobalFrameId next = descriptor.data.group.next;
    const uint32_t generation = descriptor.data.group.generation;

    descriptor = FrameDesc::free(generation);
    push_free(*arena,index);
    group.head_ = next;
    --group.page_count_;
    --outstanding_pages_;
    --outstanding_group_pages_;
}

auto Pmm::release(OwnedPageGroup& group) noexcept ->void {
    KASSERT(group.owner_.get() == this);
    KASSERT(group.id_.valid());
    KASSERT(!group.extension_active_);

    while(group.page_count_ != 0){
        release_group_head(group);
    }

    kernel::sync::IrqLockGuard guard{lock_};
    KASSERT(!group.head_.valid());
    KASSERT(outstanding_groups_ != 0);

    --outstanding_groups_;
    group.disarm();
}

auto Pmm::take_boot_reservation() noexcept -> libk::optional<BootReservation> {
    kernel::sync::IrqLockGuard guard{lock_};
    for (size_t id = 0; id < reservations_.size(); ++id) {
        auto& record = reservations_[id];
        if (record.state != ReservationState::Available) {
            continue;
        }
        record.state = ReservationState::Issued;
        ++issued_reservations_;
        BootReservation reservation{*this, id, record.range};
        return libk::optional<BootReservation>{libk::move(reservation)};
    }
    return libk::nullopt;
}

auto Pmm::take_boot_reservation_for(PageRange range) noexcept
    -> libk::optional<BootReservation> {
    kernel::sync::IrqLockGuard guard{lock_};
    if (!range.valid()) {
        return libk::nullopt;
    }
    for (size_t id = 0; id < reservations_.size(); ++id) {
        auto& record = reservations_[id];
        if (record.state != ReservationState::Available
            || record.range.first() != range.first()
            || record.range.page_count() != range.page_count()) {
            continue;
        }
        record.state = ReservationState::Issued;
        ++issued_reservations_;
        BootReservation reservation{*this, id, record.range};
        return libk::optional<BootReservation>{libk::move(reservation)};
    }
    return libk::nullopt;
}

auto Pmm::reclaim(BootReservation&& reservation) noexcept -> ReclaimResult {
    kernel::sync::IrqLockGuard guard{lock_};
    if (reservation.owner_.get() != this) {
        return libk::unexpected(BootReclaimError::WrongOwner);
    }
    if (reservation.id_ >= reservations_.size()
        || reservations_[reservation.id_].state != ReservationState::Issued) {
        return libk::unexpected(BootReclaimError::InvalidReservation);
    }

    const ReservationId id{reservation.id_};
    for (const Page page : reservation.range_) {
        Arena* arena = find_arena(page);
        if (arena == nullptr) {
            return libk::unexpected(BootReclaimError::InvalidReservation);
        }
        const FrameDesc& descriptor = descriptor_at(*arena, index_of(*arena, page));
        if (descriptor.state != FrameDescState::Reserved
            || descriptor.data.reserved.reservation != id) {
            return libk::unexpected(BootReclaimError::InvalidReservation);
        }
    }

    for (const Page page : reservation.range_) {
        Arena& arena = *find_arena(page);
        const FrameIndex index = index_of(arena, page);
        FrameDesc& descriptor = descriptor_at(arena, index);
        descriptor = FrameDesc::free(
            descriptor.data.reserved.generation);
        push_free(arena, index);
    }

    reservations_[reservation.id_].state = ReservationState::Reclaimed;
    --issued_reservations_;
    const size_t reclaimed = reservation.range_.page_count();
    reservation.disarm();
    KASSERT(verify_invariants_unlocked());
    return libk::expected(reclaimed);
}

auto Pmm::cancel(BootReservation& reservation) noexcept -> void {
    kernel::sync::IrqLockGuard guard{lock_};
    KASSERT(reservation.owner_.get() == this);
    KASSERT(reservation.id_ < reservations_.size());
    ReservationRecord& record = reservations_[reservation.id_];
    KASSERT(record.state == ReservationState::Issued);
    record.state = ReservationState::Available;
    --issued_reservations_;
    reservation.disarm();
}

auto Pmm::contains(Page page) const noexcept -> bool {
    kernel::sync::IrqLockGuard guard{lock_};
    return find_arena(page) != nullptr;
}

auto Pmm::state_of(Page page) const noexcept -> QueryResult {
    kernel::sync::IrqLockGuard guard{lock_};
    const Arena* arena = find_arena(page);
    if (arena == nullptr) {
        return libk::unexpected(QueryError::NotManaged);
    }
    return libk::expected(public_state_of(
        descriptor_at(*arena, index_of(*arena, page)).state));
}

auto Pmm::free_page_count() const noexcept -> size_t {
    kernel::sync::IrqLockGuard guard{lock_};
    size_t count = 0;
    for (const auto& arena : arenas_) {
        count += arena.free_count;
    }
    return count;
}

auto Pmm::arena_count() const noexcept -> size_t {
    kernel::sync::IrqLockGuard guard{lock_};
    return arenas_.size();
}

auto Pmm::metadata_page_count() const noexcept -> size_t {
    kernel::sync::IrqLockGuard guard{lock_};
    size_t count = 0;
    for (const auto& arena : arenas_) {
        count += arena.descriptor_storage.page_count();
    }
    return count;
}

auto Pmm::stats() const noexcept -> PmmStats {
    kernel::sync::IrqLockGuard guard{lock_};
    PmmStats result{
        .arena_count = arenas_.size(),
        .boot_reservations = reservations_.size(),
    };
    for (const auto& arena : arenas_) {
        result.metadata_pages += arena.descriptor_storage.page_count();
        for (size_t offset = 0; offset < arena.range.page_count(); ++offset) {
            switch (descriptor_at(arena, FrameIndex{offset}).state) {
            case FrameDescState::Reserved:
                ++result.reserved_pages;
                break;
            case FrameDescState::Free:
                ++result.free_pages;
                break;
            case FrameDescState::AllocatedIndividual:
            case FrameDescState::AllocatedGroup:
                ++result.allocated_pages;
                break;
            }
        }
    }
    size_t indexed_free{};
    for (const auto& arena : arenas_) {
        indexed_free += arena.free_count;
    }
    KASSERT(result.free_pages == indexed_free);
    KASSERT(result.allocated_pages == outstanding_pages_);
    return result;
}

auto Pmm::verify(const Arena& arena) const noexcept -> bool {
    size_t indexed = 0;
    FrameIndex current = arena.free_head;
    while (current.valid()) {
        if (current.raw() >= arena.range.page_count() || indexed >= arena.range.page_count()) {
            return false;
        }
        const FrameDesc& descriptor = descriptor_at(arena, current);
        if (descriptor.state != FrameDescState::Free) {
            return false;
        }
        current = descriptor.data.free.next;
        ++indexed;
    }

    size_t free = 0;
    for (size_t offset = 0; offset < arena.range.page_count(); ++offset) {
        const FrameDesc& descriptor = descriptor_at(arena, FrameIndex{offset});
        if (descriptor.state == FrameDescState::Free) {
            ++free;
        }
    }
    return indexed == free && indexed == arena.free_count;
}

auto Pmm::verify_invariants() const noexcept -> bool {
    kernel::sync::IrqLockGuard guard{lock_};
    return verify_invariants_unlocked();
}

auto Pmm::verify_invariants_unlocked() const noexcept -> bool {
    size_t allocated = 0;
    size_t grouped = 0;
    size_t issued = 0;

    for (size_t index = 0; index < arenas_.size(); ++index) {
        const Arena& arena = arenas_[index];
        if (!arena.range.valid()
            || !arena.descriptor_storage.valid()
            || !verify(arena)) {
            return false;
        }

        bool storage_is_managed = false;
        for (const auto& owner : arenas_) {
            storage_is_managed = storage_is_managed
                || owner.range.contains(arena.descriptor_storage);
        }
        if (!storage_is_managed) {
            return false;
        }
        for (const Page page : arena.descriptor_storage) {
            const Arena* owner = find_arena(page);
            if (owner == nullptr
                || descriptor_at(*owner, index_of(*owner, page)).state
                    != FrameDescState::Reserved) {
                return false;
            }
        }

        for (size_t offset = 0; offset < arena.range.page_count(); ++offset) {
            const FrameDesc& descriptor = descriptor_at(arena, FrameIndex{offset});

            if (descriptor.state == FrameDescState::AllocatedIndividual) {
                if (descriptor.data.individual.generation == 0) {
                    return false;
                }
                ++allocated;
            } else if (descriptor.state == FrameDescState::AllocatedGroup) {
                if (!descriptor.data.group.group.valid()) {
                    return false;
                }
                if (descriptor.data.group.next.valid()) {
                    const Page next_page = page_from(
                        descriptor.data.group.next);
                    const Arena* next_arena = find_arena(next_page);
                    if (next_arena == nullptr) {
                        return false;
                    }
                    const FrameDesc& next = descriptor_at(
                        *next_arena, index_of(*next_arena, next_page));
                    if (next.state != FrameDescState::AllocatedGroup
                        || next.data.group.group
                            != descriptor.data.group.group) {
                        return false;
                    }
                }
                ++allocated;
                ++grouped;
            } else if (descriptor.state == FrameDescState::Reserved
                       && descriptor.data.reserved.reservation.valid()) {
                const ReservationId reservation =
                    descriptor.data.reserved.reservation;
                if (reservation.raw() >= reservations_.size()
                    || reservations_[reservation.raw()].state
                        == ReservationState::Reclaimed) {
                    return false;
                }
            }
        }

        for (size_t other = index + 1; other < arenas_.size(); ++other) {
            if (arena.range.intersects(arenas_[other].range)
                || arena.descriptor_storage.intersects(
                    arenas_[other].descriptor_storage)) {
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
        if (reservation.state == ReservationState::Reclaimed) {
            continue;
        }
        for (const Page page : reservation.range) {
            const Arena* arena = find_arena(page);
            if (arena == nullptr) {
                return false;
            }
            const FrameDesc& descriptor = descriptor_at(*arena, index_of(*arena, page));
            if (descriptor.state != FrameDescState::Reserved
                || descriptor.data.reserved.reservation != ReservationId{id}) {
                return false;
            }
        }
    }

    return allocated == outstanding_pages_
        && grouped == outstanding_group_pages_
        && issued == issued_reservations_;
}

} // namespace kernel::mm
