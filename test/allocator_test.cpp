#include <test/test.hpp>

#include <arch/address_layout.hpp>
#include <arch/page_table.hpp>
#include <arch/riscv64/cpu/csr.hpp>
#include <arch/riscv64/mmu/range_map.hpp>
#include <arch/riscv64/mmu/sv39_builder.hpp>
#include <arch/riscv64/mmu/sv39_editor.hpp>
#include <libk/concepts.hpp>
#include <libk/inplace_vector.hpp>
#include <libk/manual_lifetime.hpp>
#include <libk/noncopyable.hpp>
#include <libk/mem.h>
#include <libk/utility.hpp>
#include <mm/pmm.hpp>
#include <mm/translation.hpp>
#include <platform/memory_layout.hpp>

namespace {

namespace riscv64 = arch::riscv64;

constinit libk::ManualLifetime<kernel::mm::Pmm> primary_memory_storage{};
constinit libk::ManualLifetime<kernel::mm::Pmm> secondary_memory_storage{};
constinit libk::ManualLifetime<kernel::mm::DirectMap> primary_direct_map{};
constinit libk::ManualLifetime<kernel::mm::DirectMap> secondary_direct_map{};

inline constexpr size_t test_pages = 512;
alignas(kernel::mm::page_size) uint8_t test_ram[test_pages * kernel::mm::page_size]{};

[[nodiscard]] auto direct_storage_for(
    libk::ManualLifetime<kernel::mm::Pmm>& storage) noexcept
    -> libk::ManualLifetime<kernel::mm::DirectMap>& {
    return &storage == &primary_memory_storage
        ? primary_direct_map
        : secondary_direct_map;
}

class PmmFixture : private libk::noncopyable_nonmovable {
  public:
    PmmFixture(
        libk::ManualLifetime<kernel::mm::Pmm>& storage,
        kernel::mm::RegionList&& memory_map) noexcept
        : storage_(storage),
          direct_(direct_storage_for(storage)),
          initialization_(initialize(libk::move(memory_map))) {}

    ~PmmFixture() noexcept {
        storage_.reset();
        direct_.reset();
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(initialization_);
    }

    [[nodiscard]] auto error() const noexcept -> kernel::mm::PmmInitError {
        return initialization_.error();
    }

    [[nodiscard]] auto memory(this auto& self) noexcept -> decltype(auto) {
        return *self.storage_;
    }

  private:
    [[nodiscard]] auto initialize(kernel::mm::RegionList&& memory_map) noexcept
        -> kernel::mm::Pmm::InitializationResult {
        const auto direct = kernel::mm::DirectMap::initialize_in(
            direct_,
            memory_map,
            kernel::mm::DirectMapLayout{
                .physical_base = kernel::mm::PhysAddr{
                    platform::memory::linked_physical(kernel::mm::VirtAddr{
                        reinterpret_cast<uintptr_t>(test_ram)})->raw()},
                .virtual_base = kernel::mm::VirtAddr{
                    reinterpret_cast<uintptr_t>(test_ram)},
                .window_size = sizeof(test_ram),
            });
        if (!direct) {
            return libk::unexpected(kernel::mm::PmmInitError::InvalidRegion);
        }
        return kernel::mm::Pmm::initialize_in(
            storage_, *direct_, libk::move(memory_map));
    }

    libk::ManualLifetime<kernel::mm::Pmm>& storage_;
    libk::ManualLifetime<kernel::mm::DirectMap>& direct_;
    kernel::mm::Pmm::InitializationResult initialization_;
};

[[nodiscard]] auto page_at(size_t offset) noexcept -> kernel::mm::Page {
    const auto base = platform::memory::linked_physical(kernel::mm::VirtAddr{
        reinterpret_cast<uintptr_t>(test_ram)});
    KASSERT(base);
    const auto address = base->checked_add(offset * kernel::mm::page_size);
    KASSERT(address);
    return *kernel::mm::Page::from_base(*address);
}

[[nodiscard]] auto page_range(size_t offset, size_t pages) noexcept
    -> kernel::mm::PageRange {
    return kernel::mm::PageRange{page_at(offset), pages};
}

[[nodiscard]] auto append_region(
    kernel::mm::RegionList& map,
    size_t offset,
    size_t pages,
    kernel::mm::RegionKind kind) noexcept -> bool {
    return map.try_emplace_back(kernel::mm::Region{page_range(offset, pages), kind});
}

[[nodiscard]] auto make_available_map(size_t pages = 64) noexcept
    -> kernel::mm::RegionList {
    kernel::mm::RegionList map{};
    (void)append_region(map, 0, pages, kernel::mm::RegionKind::AvailableRam);
    return map;
}

static_assert(
    !libk::ConstructibleFrom<riscv64::TablePage>);

static_assert(
    !libk::ConstructibleFrom<
        riscv64::TablePage,
        const riscv64::TablePage&>);

static_assert(
    !libk::ConstructibleFrom<
        riscv64::TablePage,
        riscv64::TablePage&&>);

consteval auto sv39_pte_representation_contract() noexcept -> bool {
    constexpr auto page =
        kernel::mm::Page{kernel::mm::Pfn{0x12345}};

    constexpr auto invalid_page =
        kernel::mm::Page{
            kernel::mm::Pfn{~uintptr_t{0}}};

    const auto non_leaf =
        riscv64::Pte::non_leaf(page);

    if (!non_leaf
        || non_leaf->raw()
            != ((uint64_t{0x12345} << 10) | uint64_t{1})
        || !non_leaf->is_non_leaf()
        || non_leaf->is_leaf()) {
        return false;
    }

    const auto decoded = non_leaf->next_table_page();
    if (!decoded || *decoded != page) {
        return false;
    }

    const auto leaf =
        riscv64::Pte::leaf_4k(
            page,
            riscv64::PtePerm::supervisor_rw());
    const auto user_rx =
        riscv64::Pte::leaf_4k(
            page,
            riscv64::PtePerm::user_rx());
    const auto user_rw =
        riscv64::Pte::leaf_4k(
            page,
            riscv64::PtePerm::user_rw());

    constexpr uint64_t expected_leaf =
        (uint64_t{0x12345} << 10)
        | (uint64_t{1} << 0)
        | (uint64_t{1} << 1)
        | (uint64_t{1} << 2)
        | (uint64_t{1} << 6)
        | (uint64_t{1} << 7);

    const auto leaf_page = leaf
        ? leaf->leaf_page()
        : libk::nullopt;

    constexpr uint64_t global_bit = uint64_t{1} << 5;
    constexpr uint64_t rsw_bits = uint64_t{3} << 8;
    constexpr uint64_t reserved_high = uint64_t{1} << 63;
    const auto global_leaf = riscv64::Pte::from_raw(
        expected_leaf | global_bit);
    const auto rsw_leaf = riscv64::Pte::from_raw(
        expected_leaf | rsw_bits);
    const auto global_rsw_branch = riscv64::Pte::from_raw(
        non_leaf->raw() | global_bit | rsw_bits);
    const auto reserved_leaf = riscv64::Pte::from_raw(
        expected_leaf | reserved_high);
    const auto reserved_branch = riscv64::Pte::from_raw(
        non_leaf->raw() | reserved_high);
    const auto write_without_read = riscv64::Pte::from_raw(
        uint64_t{1} | (uint64_t{1} << 2));

    return leaf
        && leaf->raw() == expected_leaf
        && leaf->is_leaf()
        && leaf_page
        && *leaf_page == page
        && leaf->has_permissions(
            riscv64::PtePerm::supervisor_rw())
        && !leaf->has_permissions(
            riscv64::PtePerm::supervisor_ro())
        && user_rx
        && user_rx->has_permissions(riscv64::PtePerm::user_rx())
        && user_rx->raw()
            == ((uint64_t{0x12345} << 10)
                | (uint64_t{1} << 0)
                | (uint64_t{1} << 1)
                | (uint64_t{1} << 3)
                | (uint64_t{1} << 4)
                | (uint64_t{1} << 6)
                | (uint64_t{1} << 7))
        && user_rw
        && user_rw->has_permissions(riscv64::PtePerm::user_rw())
        && !leaf->is_non_leaf()
        && !non_leaf->leaf_page()
        && !leaf->next_table_page()
        && global_leaf.is_leaf()
        && global_leaf.leaf_page()
        && rsw_leaf.is_leaf()
        && rsw_leaf.leaf_page()
        && global_rsw_branch.is_non_leaf()
        && global_rsw_branch.next_table_page()
        && reserved_leaf.is_leaf()
        && !reserved_leaf.leaf_page()
        && reserved_branch.is_non_leaf()
        && !reserved_branch.next_table_page()
        && !write_without_read.is_leaf()
        && !write_without_read.is_non_leaf()
        && !riscv64::Pte::non_leaf(invalid_page)
        && !riscv64::Pte::leaf_4k(
            invalid_page,
            riscv64::PtePerm::supervisor_rw());
}

static_assert(sv39_pte_representation_contract());

consteval auto satp_representation_contract() noexcept -> bool {
    constexpr usize max_ppn =
        (usize{1} << riscv64::Satp::PPN_WIDTH) - 1;
    constexpr usize max_asid =
        (usize{1} << riscv64::Satp::ASID_WIDTH) - 1;
    const auto maximum = riscv64::Satp::try_make_sv39(
        max_ppn,
        max_asid);
    return maximum
        && riscv64::Satp::mode(*maximum) == riscv64::Satp::MODE_SV39
        && riscv64::Satp::ppn(*maximum) == max_ppn
        && riscv64::Satp::asid(*maximum) == max_asid
        && !riscv64::Satp::try_make_sv39(max_ppn + 1)
        && !riscv64::Satp::try_make_sv39(0, max_asid + 1);
}

static_assert(satp_representation_contract());

bool test_allocate_owner_releases_on_destruction(const TestContext&) noexcept {
    PmmFixture fixture{primary_memory_storage, make_available_map()};
    if (!fixture) {
        return false;
    }
    auto& memory = fixture.memory();
    const size_t initial_free = memory.free_page_count();
    kernel::mm::Page page{};

    {
        auto allocation = memory.allocate_page();
        if (!allocation) {
            return false;
        }
        auto owner = libk::move(allocation).value();
        page = owner.page();
        const auto state = memory.state_of(page);
        if (!state || state.value() != kernel::mm::PageState::Allocated
            || memory.free_page_count() + 1 != initial_free) {
            return false;
        }
    }

    const auto state = memory.state_of(page);
    return state
        && state.value() == kernel::mm::PageState::Free
        && memory.free_page_count() == initial_free
        && memory.verify_invariants();
}

bool test_owned_page_move_transfers_release_authority(const TestContext&) noexcept {
    PmmFixture fixture{primary_memory_storage, make_available_map()};
    if (!fixture) {
        return false;
    }
    auto& memory = fixture.memory();
    auto allocation = memory.allocate_page();
    if (!allocation) {
        return false;
    }
    auto first = libk::move(allocation).value();
    const kernel::mm::Page page = first.page();
    kernel::mm::OwnedPage second{libk::move(first)};
    if (first || !second || second.page() != page) {
        return false;
    }
    second.reset();
    const auto state = memory.state_of(page);
    return !second && state && state.value() == kernel::mm::PageState::Free;
}

bool test_metadata_is_reserved_and_external_to_free_index(const TestContext&) noexcept {
    PmmFixture fixture{primary_memory_storage, make_available_map()};
    if (!fixture) {
        return false;
    }
    auto& memory = fixture.memory();
    const auto stats = memory.stats();
    const auto first = memory.state_of(page_at(0));
    return first
        && first.value() == kernel::mm::PageState::Reserved
        && stats.arena_count == 1
        && stats.metadata_pages != 0
        && stats.reserved_pages == stats.metadata_pages
        && stats.free_pages + stats.reserved_pages == 64
        && memory.verify_invariants();
}

bool test_boot_reservation_requires_explicit_consumption(const TestContext&) noexcept {
    kernel::mm::RegionList map{};
    if (!append_region(map, 0, 48, kernel::mm::RegionKind::AvailableRam)
        || !append_region(map, 48, 3, kernel::mm::RegionKind::ReclaimableBootData)
        || !append_region(map, 51, 13, kernel::mm::RegionKind::AvailableRam)) {
        return false;
    }
    PmmFixture fixture{primary_memory_storage, libk::move(map)};
    if (!fixture) {
        return false;
    }
    auto& memory = fixture.memory();
    const kernel::mm::Page reclaimed_page = page_at(48);
    const auto before = memory.state_of(reclaimed_page);
    const size_t initial_free = memory.free_page_count();
    auto reservation = memory.take_boot_reservation();
    if (!before || before.value() != kernel::mm::PageState::Reserved
        || !reservation || reservation->range().page_count() != 3) {
        return false;
    }
    auto reclaimed = memory.reclaim(libk::move(*reservation));
    const auto after = memory.state_of(reclaimed_page);
    return reclaimed
        && reclaimed.value() == 3
        && after
        && after.value() == kernel::mm::PageState::Free
        && memory.free_page_count() == initial_free + 3
        && !memory.take_boot_reservation()
        && memory.verify_invariants();
}

bool test_dropped_reservation_can_be_taken_again(const TestContext&) noexcept {
    kernel::mm::RegionList map{};
    if (!append_region(map, 0, 48, kernel::mm::RegionKind::AvailableRam)
        || !append_region(map, 48, 2, kernel::mm::RegionKind::ReclaimableBootData)) {
        return false;
    }
    PmmFixture fixture{primary_memory_storage, libk::move(map)};
    if (!fixture) {
        return false;
    }
    auto& memory = fixture.memory();
    {
        auto reservation = memory.take_boot_reservation();
        if (!reservation) {
            return false;
        }
    }
    return static_cast<bool>(memory.take_boot_reservation());
}

bool test_exact_boot_reservation_handoff(const TestContext&) noexcept {
    kernel::mm::RegionList map{};
    if (!append_region(map, 0, 48, kernel::mm::RegionKind::AvailableRam)
        || !append_region(map, 48, 2, kernel::mm::RegionKind::ReclaimableBootData)
        || !append_region(map, 50, 2, kernel::mm::RegionKind::AvailableRam)
        || !append_region(map, 52, 3, kernel::mm::RegionKind::ReclaimableBootData)
        || !append_region(map, 55, 9, kernel::mm::RegionKind::AvailableRam)) {
        return false;
    }
    PmmFixture fixture{primary_memory_storage, libk::move(map)};
    if (!fixture) {
        return false;
    }
    auto& memory = fixture.memory();
    const kernel::mm::PageRange target = page_range(52, 3);
    auto selected = memory.take_boot_reservation_for(target);
    if (!selected
        || selected->range().first() != target.first()
        || selected->range().page_count() != target.page_count()
        || memory.take_boot_reservation_for(page_range(51, 3))) {
        return false;
    }
    auto remaining = memory.take_boot_reservation();
    if (!remaining
        || remaining->range().first() != page_at(48)
        || remaining->range().page_count() != 2) {
        return false;
    }
    const auto selected_reclaimed = memory.reclaim(libk::move(*selected));
    const auto remaining_reclaimed = memory.reclaim(libk::move(*remaining));
    return selected_reclaimed && selected_reclaimed.value() == 3
        && remaining_reclaimed && remaining_reclaimed.value() == 2
        && memory.verify_invariants();
}

bool test_reservation_authority_is_bound_to_its_owner(const TestContext&) noexcept {
    kernel::mm::RegionList first_map{};
    kernel::mm::RegionList second_map{};
    if (!append_region(first_map, 0, 48, kernel::mm::RegionKind::AvailableRam)
        || !append_region(first_map, 48, 2, kernel::mm::RegionKind::ReclaimableBootData)
        || !append_region(first_map, 50, 14, kernel::mm::RegionKind::AvailableRam)
        || !append_region(second_map, 96, 64, kernel::mm::RegionKind::AvailableRam)) {
        return false;
    }
    PmmFixture first_fixture{
        primary_memory_storage, libk::move(first_map)};
    if (!first_fixture) {
        return false;
    }
    PmmFixture second_fixture{
        secondary_memory_storage, libk::move(second_map)};
    if (!second_fixture) {
        return false;
    }
    auto& first = first_fixture.memory();
    auto& second = second_fixture.memory();
    auto reservation = first.take_boot_reservation();
    if (!reservation) {
        return false;
    }
    const auto reclaimed = second.reclaim(libk::move(*reservation));
    return !reclaimed
        && reclaimed.error() == kernel::mm::BootReclaimError::WrongOwner
        && first.verify_invariants()
        && second.verify_invariants();
}

bool test_discontiguous_ram_forms_multiple_arenas(const TestContext&) noexcept {
    kernel::mm::RegionList map{};
    if (!append_region(map, 96, 32, kernel::mm::RegionKind::AvailableRam)
        || !append_region(map, 0, 64, kernel::mm::RegionKind::AvailableRam)) {
        return false;
    }
    PmmFixture fixture{primary_memory_storage, libk::move(map)};
    if (!fixture) {
        return false;
    }
    auto& memory = fixture.memory();
    const auto stats = memory.stats();
    return stats.arena_count == 2
        && stats.metadata_pages >= 2
        && stats.free_pages + stats.reserved_pages == 96
        && memory.verify_invariants();
}

bool test_foreign_page_and_mmio_are_not_managed(const TestContext&) noexcept {
    kernel::mm::RegionList map = make_available_map();
    if (!append_region(map, 96, 2, kernel::mm::RegionKind::Mmio)) {
        return false;
    }
    PmmFixture fixture{primary_memory_storage, libk::move(map)};
    if (!fixture) {
        return false;
    }
    auto& memory = fixture.memory();
    const kernel::mm::Page foreign = page_at(80);
    const kernel::mm::Page mmio = page_at(96);
    return !memory.contains(foreign)
        && !memory.contains(mmio)
        && !memory.state_of(foreign)
        && !memory.state_of(mmio);
}

bool test_exhaustion_preserves_ledger_index_equivalence(const TestContext&) noexcept {
    PmmFixture fixture{
        primary_memory_storage, make_available_map(32)};
    if (!fixture) {
        return false;
    }
    auto& memory = fixture.memory();
    libk::InplaceVector<kernel::mm::OwnedPage, 32> owners{};
    const size_t available = memory.free_page_count();
    for (size_t index = 0; index < available; ++index) {
        auto allocation = memory.allocate_page();
        if (!allocation
            || !owners.try_push_back(libk::move(allocation).value())
            || !memory.verify_invariants()) {
            return false;
        }
    }
    if (memory.allocate_page()) {
        return false;
    }
    owners.clear();
    return memory.free_page_count() == available && memory.verify_invariants();
}

bool test_empty_page_group_move_transfers_authority(const TestContext&) noexcept {
    PmmFixture fixture{primary_memory_storage, make_available_map()};
    if (!fixture) {
        return false;
    }
    auto& memory = fixture.memory();
    const size_t initial_free = memory.free_page_count();

    auto first = memory.make_page_group();
    kernel::mm::OwnedPageGroup second{libk::move(first)};
    if (first || !second || second.page_count() != 0) {
        return false;
    }

    second.reset();
    return !second
        && memory.free_page_count() == initial_free
        && memory.verify_invariants();
}

bool test_page_group_rolls_back_same_arena_pages(const TestContext&) noexcept {
    PmmFixture fixture{primary_memory_storage, make_available_map()};
    if (!fixture) {
        return false;
    }
    auto& memory = fixture.memory();
    const size_t initial_free = memory.free_page_count();
    kernel::mm::Page pages[3]{};

    {
        auto group = memory.make_page_group();
        {
            auto extension = group.extend();
            for (size_t index = 0; index < 3; ++index) {
                auto allocation = extension.allocate_page();
                if (!allocation) {
                    return false;
                }
                pages[index] = allocation.value();
            }
            extension.commit();
        }
        if (group.page_count() != 3
            || memory.free_page_count() + 3 != initial_free
            || !memory.verify_invariants()) {
            return false;
        }
    }

    for (const kernel::mm::Page page : pages) {
        const auto state = memory.state_of(page);
        if (!state || state.value() != kernel::mm::PageState::Free) {
            return false;
        }
    }
    return memory.free_page_count() == initial_free
        && memory.verify_invariants();
}

bool test_empty_page_group_extension_releases_borrow(
    const TestContext&) noexcept {
    PmmFixture fixture{
        primary_memory_storage,
        make_available_map()};
    if (!fixture) {
        return false;
    }

    auto& memory = fixture.memory();
    const size_t initial_free = memory.free_page_count();
    auto group = memory.make_page_group();

    {
        auto extension = group.extend();
    }

    {
        auto extension = group.extend();
        auto allocation = extension.allocate_page();
        if (!allocation) {
            return false;
        }
        extension.commit();
    }

    return group.page_count() == 1
        && memory.free_page_count() + 1 == initial_free
        && memory.verify_invariants();
}

bool test_page_group_extension_rolls_back_only_new_prefix(
    const TestContext&) noexcept {
    PmmFixture fixture{
        primary_memory_storage,
        make_available_map()};
    if (!fixture) {
        return false;
    }

    auto& memory = fixture.memory();
    const size_t initial_free = memory.free_page_count();
    auto group = memory.make_page_group();
    kernel::mm::Page retained{};
    kernel::mm::Page rolled_back[3]{};

    {
        auto extension = group.extend();
        auto allocation = extension.allocate_page();
        if (!allocation) {
            return false;
        }
        retained = allocation.value();
        extension.commit();
    }

    const size_t committed_free = memory.free_page_count();
    {
        auto extension = group.extend();
        for (size_t index = 0; index < 3; ++index) {
            auto allocation = extension.allocate_page();
            if (!allocation) {
                return false;
            }
            rolled_back[index] = allocation.value();
        }

        if (group.page_count() != 4
            || memory.free_page_count() + 3 != committed_free
            || !memory.verify_invariants()) {
            return false;
        }
    }

    const auto retained_state = memory.state_of(retained);
    if (group.page_count() != 1
        || memory.free_page_count() != committed_free
        || !retained_state
        || retained_state.value() != kernel::mm::PageState::Allocated) {
        return false;
    }

    for (const kernel::mm::Page page : rolled_back) {
        const auto state = memory.state_of(page);
        if (!state || state.value() != kernel::mm::PageState::Free) {
            return false;
        }
    }

    group.reset();
    return memory.free_page_count() == initial_free
        && memory.verify_invariants();
}

bool test_page_group_chain_crosses_arenas(const TestContext&) noexcept {
    kernel::mm::RegionList map{};
    const auto first_range = page_range(0, 16);
    const auto second_range = page_range(32, 16);
    if (!map.try_emplace_back(kernel::mm::Region{
            first_range, kernel::mm::RegionKind::AvailableRam})
        || !map.try_emplace_back(kernel::mm::Region{
            second_range, kernel::mm::RegionKind::AvailableRam})) {
        return false;
    }

    PmmFixture fixture{primary_memory_storage, libk::move(map)};
    if (!fixture) {
        return false;
    }
    auto& memory = fixture.memory();
    const size_t initial_free = memory.free_page_count();
    bool saw_first = false;
    bool saw_second = false;

    {
        auto group = memory.make_page_group();
        {
            auto extension = group.extend();
            for (size_t index = 0; index < initial_free; ++index) {
                auto allocation = extension.allocate_page();
                if (!allocation) {
                    return false;
                }
                saw_first = saw_first
                    || first_range.contains(allocation.value());
                saw_second = saw_second
                    || second_range.contains(allocation.value());
            }
            if (!saw_first || !saw_second
                || group.page_count() != initial_free
                || extension.allocate_page()
                || !memory.verify_invariants()) {
                return false;
            }
            extension.commit();
        }
    }

    return memory.free_page_count() == initial_free
        && memory.verify_invariants();
}

bool test_page_group_detach_and_reattach_preserve_frame(
    const TestContext&) noexcept {
    PmmFixture fixture{primary_memory_storage, make_available_map()};
    if (!fixture) {
        return false;
    }
    auto& memory = fixture.memory();
    const usize initial_free = memory.free_page_count();
    {
        auto group = memory.make_page_group();
        auto extension = group.extend();
        const auto first = extension.allocate_page();
        const auto second = extension.allocate_page();
        if (!first || !second) {
            return false;
        }
        extension.commit();
        auto detached = group.detach(first.value());
        if (!detached
            || detached->page() != first.value()
            || group.page_count() != 1
            || memory.free_page_count() + 2 != initial_free
            || !group.attach(libk::move(*detached))
            || *detached
            || group.page_count() != 2
            || !memory.verify_invariants()) {
            return false;
        }
    }
    return memory.free_page_count() == initial_free
        && memory.verify_invariants();
}

bool test_runtime_editor_owns_private_and_shared_tables(
    const TestContext&) noexcept {
    PmmFixture fixture{
        primary_memory_storage,
        make_available_map(384)};
    if (!fixture) {
        return false;
    }
    auto& memory = fixture.memory();
    const usize initial_free = memory.free_page_count();

    {
        auto builder_result = riscv64::Sv39Builder::create(memory);
        if (!builder_result) {
            return false;
        }
        auto builder = libk::move(builder_result).value();
        constexpr usize root_span = usize{1} << 30;
        for (usize index = 256; index < 512; ++index) {
            const auto page = kernel::mm::VPage::from_base(kernel::mm::VirtAddr{
                arch::layout::direct_base + (index - 256) * root_span});
            if (!page || !builder.ensure_root_branch(*page)) {
                return false;
            }
        }
        arch::KernelRoot kernel_root = libk::move(builder).finalize();
        auto user_result = arch::UserRoot::create(kernel_root, memory);
        auto payload_result = memory.allocate_page();
        if (!user_result || !payload_result) {
            return false;
        }
        arch::UserRoot user_root = libk::move(user_result).value();
        kernel::mm::OwnedPage payload = libk::move(payload_result).value();

        const auto user_page = kernel::mm::VPage::from_base(
            kernel::mm::VirtAddr{arch::layout::low_guard_end});
        if (!user_page) {
            return false;
        }
        auto user = riscv64::Editor::user(user_root);
        if (!user.map(
                *user_page,
                payload.page(),
                riscv64::PtePerm::user_rw())) {
            return false;
        }
        const auto mapped = user.query(*user_page);
        const auto previous = user.protect(
            *user_page, riscv64::PtePerm::user_rx());
        const auto protected_leaf = user.query(*user_page);
        if (!mapped
            || mapped.value().page != payload.page()
            || mapped.value().permissions != riscv64::PtePerm::user_rw()
            || !previous
            || previous.value().permissions != riscv64::PtePerm::user_rw()
            || !protected_leaf
            || protected_leaf.value().permissions != riscv64::PtePerm::user_rx()) {
            return false;
        }

        auto unmapped = user.unmap(*user_page);
        if (!unmapped) {
            return false;
        }
        if (unmapped.value().leaf.page != payload.page()) {
            return false;
        }
        if (unmapped.value().tables.size() != 2) {
            return false;
        }
        if (user.query(*user_page)) {
            return false;
        }
        if (!memory.verify_invariants()) {
            return false;
        }
        kernel::mm::RetireBatch retired{memory};
        while (auto page = unmapped.value().tables.take()) {
            if (!retired.adopt(libk::move(*page))) {
                return false;
            }
        }
        if (retired.page_count() != 2 || !retired.release()) {
            return false;
        }

        const auto kernel_page = kernel::mm::VPage::from_base(
            kernel::mm::VirtAddr{arch::layout::direct_base});
        if (!kernel_page) {
            return false;
        }
        auto kernel = riscv64::Editor::kernel(kernel_root);
        if (!kernel.map(
                *kernel_page,
                payload.page(),
                riscv64::PtePerm::supervisor_rw())) {
            return false;
        }
        auto kernel_unmapped = kernel.unmap(*kernel_page);
        if (!kernel_unmapped
            || kernel_unmapped.value().tables.size() != 1) {
            return false;
        }
        kernel::mm::RetireBatch kernel_retired{memory};
        while (auto page = kernel_unmapped.value().tables.take()) {
            if (!kernel_retired.adopt(libk::move(*page))) {
                return false;
            }
        }
        if (!kernel_retired.release() || !memory.verify_invariants()) {
            return false;
        }
    }
    return memory.free_page_count() == initial_free
        && memory.verify_invariants();
}

bool test_direct_map_preserves_ram_independent_of_allocation_state(
    const TestContext&) noexcept {
    kernel::mm::RegionList map{};
    const auto available = page_range(0, 48);
    const auto kernel_image = page_range(48, 4);
    const auto firmware = page_range(52, 4);
    const auto reclaimable = page_range(56, 4);
    const auto available_tail = page_range(60, 20);

    if (!map.try_emplace_back(kernel::mm::Region{
            available, kernel::mm::RegionKind::AvailableRam})
        || !map.try_emplace_back(kernel::mm::Region{
            kernel_image, kernel::mm::RegionKind::KernelImage})
        || !map.try_emplace_back(kernel::mm::Region{
            firmware, kernel::mm::RegionKind::FirmwareReserved})
        || !map.try_emplace_back(kernel::mm::Region{
            reclaimable,
            kernel::mm::RegionKind::ReclaimableBootData})
        || !map.try_emplace_back(kernel::mm::Region{
            available_tail,
            kernel::mm::RegionKind::AvailableRam})) {
        return false;
    }

    PmmFixture fixture{primary_memory_storage, libk::move(map)};
    if (!fixture) {
        return false;
    }
    auto& memory = fixture.memory();
    const auto expected_ram = page_range(0, 80);

    if (memory.arena_count() != 1
        || memory.metadata_page_count() == 0
        || memory.direct_map().range_count() != 1
        || memory.direct_map().range(0).first()
            != expected_ram.first()
        || memory.direct_map().range(0).page_count()
            != expected_ram.page_count()
        || !memory.direct_map().range(0).contains(
            kernel_image.first())
        || !memory.direct_map().range(0).contains(
            firmware.first())) {
        return false;
    }

    const auto metadata_state = memory.state_of(available.first());
    if (!metadata_state
        || metadata_state.value()
            != kernel::mm::PageState::Reserved) {
        return false;
    }

    auto reservation = memory.take_boot_reservation();
    if (!reservation || reservation->range().first()
            != reclaimable.first()) {
        return false;
    }
    const auto reclaimed = memory.reclaim(libk::move(*reservation));

    return reclaimed
        && reclaimed.value() == reclaimable.page_count()
        && memory.direct_map().range_count() == 1
        && memory.direct_map().range(0).first()
            == expected_ram.first()
        && memory.direct_map().range(0).page_count()
            == expected_ram.page_count()
        && memory.verify_invariants();
}

bool test_page_table_move_transfers_complete_tree(const TestContext&) noexcept {
    PmmFixture fixture{primary_memory_storage, make_available_map()};
    if (!fixture) {
        return false;
    }
    auto& memory = fixture.memory();
    const size_t initial_free = memory.free_page_count();
    kernel::mm::Page root{};

    {
        auto builder_result = riscv64::Sv39Builder::create(memory);
        if (!builder_result || memory.free_page_count() + 1 != initial_free) {
            return false;
        }
        auto builder = libk::move(builder_result).value();
        root = builder.root_page();
        const auto virtual_page = kernel::mm::VPage::from_base(
            kernel::mm::VirtAddr{0x40000000});
        if (!virtual_page
            || !builder.map_page(
                *virtual_page,
                kernel::mm::Page{kernel::mm::Pfn{0x10000}},
                riscv64::PtePerm::supervisor_rw())
            || memory.free_page_count() + 3 != initial_free) {
            return false;
        }
        auto source = libk::move(builder).finalize();
        const auto token = source.token();
        arch::KernelRoot destination{libk::move(source)};
        const auto state = memory.state_of(root);
        if (source
            || !destination
            || destination.token() != token
            || !state
            || state.value() != kernel::mm::PageState::Allocated) {
            return false;
        }
    }

    const auto state = memory.state_of(root);
    return state
        && state.value() == kernel::mm::PageState::Free
        && memory.free_page_count() == initial_free
        && memory.verify_invariants();
}

bool test_sv39_page_table_initialization_clears_complete_frame(
    const TestContext&) noexcept {
    PmmFixture fixture{
        primary_memory_storage,
        make_available_map()};
    if (!fixture) {
        return false;
    }

    auto& memory = fixture.memory();
    size_t dirtied_pages = 0;

    for (size_t range_index = 0;
         range_index < memory.direct_map().range_count();
         ++range_index) {
        for (const kernel::mm::Page page
             : memory.direct_map().range(range_index)) {
            const auto state = memory.state_of(page);
            if (!state || state.value() != kernel::mm::PageState::Free) {
                continue;
            }

            memset(
                memory.bytes(page),
                0xa5,
                kernel::mm::page_size);
            ++dirtied_pages;
        }
    }

    if (dirtied_pages == 0) {
        return false;
    }

    auto builder_result = riscv64::Sv39Builder::create(memory);
    if (!builder_result) {
        return false;
    }

    auto builder = libk::move(builder_result).value();
    const auto* const bytes = reinterpret_cast<const uint8_t*>(
        memory.bytes(builder.root_page()));

    for (size_t index = 0; index < kernel::mm::page_size; ++index) {
        if (bytes[index] != 0) {
            return false;
        }
    }

    return memory.verify_invariants();
}

bool test_sv39_walk_allocates_only_missing_tables(
    const TestContext&) noexcept {
    PmmFixture fixture{
        primary_memory_storage,
        make_available_map()};
    if (!fixture) {
        return false;
    }

    auto& memory = fixture.memory();
    const size_t initial_free = memory.free_page_count();

    {
        auto builder_result = riscv64::Sv39Builder::create(memory);
        if (!builder_result) {
            return false;
        }
        auto builder = libk::move(builder_result).value();
        const size_t after_root = memory.free_page_count();

        const auto first = kernel::mm::VPage::from_base(
            kernel::mm::VirtAddr{0x40000000});
        const auto same_level0 = kernel::mm::VPage::from_base(
            kernel::mm::VirtAddr{0x40000000 + kernel::mm::page_size});
        const auto new_level1 = kernel::mm::VPage::from_base(
            kernel::mm::VirtAddr{0x40000000 + (uintptr_t{1} << 21)});
        const auto new_level2 = kernel::mm::VPage::from_base(
            kernel::mm::VirtAddr{0x80000000});
        if (!first || !same_level0 || !new_level1 || !new_level2) {
            return false;
        }

        if (!builder.map_page(
                *first,
                kernel::mm::Page{kernel::mm::Pfn{0x10000}},
                riscv64::PtePerm::supervisor_rw())
            || memory.free_page_count() + 2 != after_root) {
            return false;
        }

        const size_t after_first = memory.free_page_count();
        if (!builder.map_page(
                *same_level0,
                kernel::mm::Page{kernel::mm::Pfn{0x10001}},
                riscv64::PtePerm::supervisor_rw())
            || memory.free_page_count() != after_first) {
            return false;
        }

        if (!builder.map_page(
                *new_level1,
                kernel::mm::Page{kernel::mm::Pfn{0x10002}},
                riscv64::PtePerm::supervisor_ro())
            || memory.free_page_count() + 1 != after_first) {
            return false;
        }

        const size_t after_new_level1 = memory.free_page_count();
        if (!builder.map_page(
                *new_level2,
                kernel::mm::Page{kernel::mm::Pfn{0x10003}},
                riscv64::PtePerm::supervisor_rx())
            || memory.free_page_count() + 2 != after_new_level1
            || !memory.verify_invariants()) {
            return false;
        }
    }

    return memory.free_page_count() == initial_free
        && memory.verify_invariants();
}

bool test_sv39_mapping_failures_preserve_allocations(
    const TestContext&) noexcept {
    PmmFixture fixture{
        primary_memory_storage,
        make_available_map()};
    if (!fixture) {
        return false;
    }

    auto& memory = fixture.memory();
    auto builder_result = riscv64::Sv39Builder::create(memory);
    if (!builder_result) {
        return false;
    }
    auto builder = libk::move(builder_result).value();

    const auto mapped_page = kernel::mm::VPage::from_base(
        kernel::mm::VirtAddr{0x40000000});
    const auto other_page = kernel::mm::VPage::from_base(
        kernel::mm::VirtAddr{0x40000000 + kernel::mm::page_size});
    const auto noncanonical_page = kernel::mm::VPage::from_base(
        kernel::mm::VirtAddr{uintptr_t{1} << 39});
    if (!mapped_page || !other_page || !noncanonical_page) {
        return false;
    }

    const auto target =
        kernel::mm::Page{kernel::mm::Pfn{0x10000}};
    if (!builder.map_page(
            *mapped_page,
            target,
            riscv64::PtePerm::supervisor_rw())) {
        return false;
    }

    const size_t committed_free = memory.free_page_count();
    const auto conflict = builder.map_page(
        *mapped_page,
        target,
        riscv64::PtePerm::supervisor_rw());
    const auto invalid_physical = builder.map_page(
        *other_page,
        kernel::mm::Page{
            kernel::mm::Pfn{uintptr_t{1} << 44}},
        riscv64::PtePerm::supervisor_rw());
    const auto noncanonical = builder.map_page(
        *noncanonical_page,
        target,
        riscv64::PtePerm::supervisor_rw());

    return !conflict
        && conflict.error() == riscv64::MappingError::MappingConflict
        && !invalid_physical
        && invalid_physical.error()
            == riscv64::MappingError::BadPAddr
        && !noncanonical
        && noncanonical.error()
            == riscv64::MappingError::BadVAddr
        && memory.free_page_count() == committed_free
        && memory.verify_invariants();
}

bool test_sv39_missing_branch_rolls_back_partial_allocation(
    const TestContext&) noexcept {
    PmmFixture fixture{
        primary_memory_storage,
        make_available_map(16)};
    if (!fixture) {
        return false;
    }

    auto& memory = fixture.memory();
    const size_t initial_free = memory.free_page_count();

    {
        auto builder_result = riscv64::Sv39Builder::create(memory);
        if (!builder_result) {
            return false;
        }
        auto builder = libk::move(builder_result).value();
        libk::InplaceVector<kernel::mm::OwnedPage, 16> held_pages{};

        while (memory.free_page_count() > 1) {
            auto allocation = memory.allocate_page();
            if (!allocation
                || !held_pages.try_push_back(
                    libk::move(allocation).value())) {
                return false;
            }
        }

        const auto virtual_page = kernel::mm::VPage::from_base(
            kernel::mm::VirtAddr{0x40000000});
        if (!virtual_page || memory.free_page_count() != 1) {
            return false;
        }

        const auto failed = builder.map_page(
            *virtual_page,
            kernel::mm::Page{kernel::mm::Pfn{0x10000}},
            riscv64::PtePerm::supervisor_rw());
        if (failed
            || failed.error()
                != riscv64::MappingError::AllocFailed
            || memory.free_page_count() != 1
            || !memory.verify_invariants()) {
            return false;
        }

        if (!held_pages.try_pop_back()
            || memory.free_page_count() != 2) {
            return false;
        }

        if (!builder.map_page(
                *virtual_page,
                kernel::mm::Page{kernel::mm::Pfn{0x10000}},
                riscv64::PtePerm::supervisor_rw())
            || memory.free_page_count() != 0
            || !memory.verify_invariants()) {
            return false;
        }
    }

    return memory.free_page_count() == initial_free
        && memory.verify_invariants();
}

bool test_sv39_range_helpers_map_and_inspect(
    const TestContext&) noexcept {
    PmmFixture fixture{
        primary_memory_storage,
        make_available_map()};
    if (!fixture) {
        return false;
    }

    auto& memory = fixture.memory();
    auto builder_result = riscv64::Sv39Builder::create(memory);
    if (!builder_result.has_value()) {
        return false;
    }
    auto builder = libk::move(builder_result).value();

    const auto first = kernel::mm::VPage::from_base(
        kernel::mm::VirtAddr{0x50000000});
    if (!first.has_value()) {
        return false;
    }

    const kernel::mm::PageRange physical{
        kernel::mm::Page{kernel::mm::Pfn{0x20000}},
        3,
    };
    const auto mapped = riscv64::map_range(
        builder,
        first.value(),
        physical,
        riscv64::PtePerm::supervisor_rw());
    if (!mapped.has_value()) {
        return false;
    }

    const auto third = first.value().checked_add(2);
    return third.has_value()
        && riscv64::maps_range(
            builder,
            first.value(),
            physical,
            riscv64::PtePerm::supervisor_rw())
        && riscv64::maps_page(
            builder,
            third.value(),
            kernel::mm::Page{kernel::mm::Pfn{0x20002}},
            riscv64::PtePerm::supervisor_rw())
        && !riscv64::maps_page(
            builder,
            third.value(),
            kernel::mm::Page{kernel::mm::Pfn{0x20002}},
            riscv64::PtePerm::supervisor_ro())
        && memory.verify_invariants();
}

bool test_sv39_range_helper_keeps_mapped_prefix_on_failure(
    const TestContext&) noexcept {
    PmmFixture fixture{
        primary_memory_storage,
        make_available_map()};
    if (!fixture) {
        return false;
    }

    auto& memory = fixture.memory();
    auto builder_result = riscv64::Sv39Builder::create(memory);
    if (!builder_result.has_value()) {
        return false;
    }
    auto builder = libk::move(builder_result).value();

    const auto first = kernel::mm::VPage::from_base(
        kernel::mm::VirtAddr{0x60000000});
    if (!first.has_value()) {
        return false;
    }
    const auto second = first.value().checked_add(1);
    if (!second.has_value()) {
        return false;
    }

    const kernel::mm::Page already_mapped{kernel::mm::Pfn{0x30fff}};
    if (!builder.map_page(
            second.value(),
            already_mapped,
            riscv64::PtePerm::supervisor_rw()).has_value()) {
        return false;
    }

    const kernel::mm::PageRange physical{
        kernel::mm::Page{kernel::mm::Pfn{0x30000}},
        2,
    };
    const auto failed = riscv64::map_range(
        builder,
        first.value(),
        physical,
        riscv64::PtePerm::supervisor_rw());

    return !failed.has_value()
        && failed.error() == riscv64::MappingError::MappingConflict
        && riscv64::maps_page(
            builder,
            first.value(),
            kernel::mm::Page{kernel::mm::Pfn{0x30000}},
            riscv64::PtePerm::supervisor_rw())
        && riscv64::maps_page(
            builder,
            second.value(),
            already_mapped,
            riscv64::PtePerm::supervisor_rw())
        && !riscv64::maps_range(
            builder,
            first.value(),
            physical,
            riscv64::PtePerm::supervisor_rw())
        && memory.verify_invariants();
}

bool test_initial_page_table_exhaustion_rolls_back(
    const TestContext&) noexcept {
    PmmFixture fixture{
        primary_memory_storage,
        make_available_map(3)};
    if (!fixture) {
        return false;
    }

    auto& memory = fixture.memory();
    const size_t initial_free = memory.free_page_count();
    const auto result = arch::build_kernel_root(memory);

    return !result
        && result.error()
            == arch::RootError::InsufficientMemory
        && memory.free_page_count() == initial_free
        && memory.verify_invariants();
}

bool test_initial_page_table_unrepresentable_range_rolls_back(
    const TestContext&) noexcept {
    constexpr auto unrepresentable_range = kernel::mm::PageRange{
        kernel::mm::Page{kernel::mm::Pfn{uintptr_t{1} << 44}},
        1,
    };

    auto map = make_available_map();
    if (!map.try_emplace_back(kernel::mm::Region{
            unrepresentable_range,
            kernel::mm::RegionKind::AvailableRam})) {
        return false;
    }
    const auto base = platform::memory::linked_physical(kernel::mm::VirtAddr{
        reinterpret_cast<uintptr_t>(test_ram)});
    KASSERT(base);
    const auto result = kernel::mm::DirectMap::initialize_in(
        secondary_direct_map,
        map,
        kernel::mm::DirectMapLayout{
            .physical_base = *base,
            .virtual_base = kernel::mm::VirtAddr{
                reinterpret_cast<uintptr_t>(test_ram)},
            .window_size = sizeof(test_ram),
        });
    secondary_direct_map.reset();
    return !result && result.error() == kernel::mm::DirectMapError::OutsideWindow;
}

bool test_empty_map_is_rejected(const TestContext&) noexcept {
    kernel::mm::RegionList map{};
    PmmFixture fixture{primary_memory_storage, libk::move(map)};
    return !fixture
        && fixture.error() == kernel::mm::PmmInitError::InvalidRegion;
}

bool test_invalid_region_is_rejected(const TestContext&) noexcept {
    kernel::mm::RegionList map{};
    (void)append_region(map, 0, 0, kernel::mm::RegionKind::AvailableRam);
    PmmFixture fixture{primary_memory_storage, libk::move(map)};
    return !fixture
        && fixture.error() == kernel::mm::PmmInitError::InvalidRegion;
}

bool test_overlapping_regions_are_rejected(const TestContext&) noexcept {
    auto map = make_available_map();
    if (!append_region(map, 1, 2, kernel::mm::RegionKind::ReclaimableBootData)) {
        return false;
    }
    PmmFixture fixture{primary_memory_storage, libk::move(map)};
    return !fixture
        && fixture.error() == kernel::mm::PmmInitError::InvalidRegion;
}

bool test_available_ram_is_required(const TestContext&) noexcept {
    kernel::mm::RegionList map{};
    if (!append_region(map, 0, 2, kernel::mm::RegionKind::KernelImage)) {
        return false;
    }
    PmmFixture fixture{primary_memory_storage, libk::move(map)};
    return !fixture
        && fixture.error() == kernel::mm::PmmInitError::NoAvailableRam;
}

bool test_metadata_capacity_failure_is_explicit(const TestContext&) noexcept {
    kernel::mm::RegionList map{};
    if (!append_region(map, 0, 1, kernel::mm::RegionKind::AvailableRam)) {
        return false;
    }
    const auto remote = kernel::mm::PageRange{
        kernel::mm::Page{kernel::mm::Pfn{0x90000000 / kernel::mm::page_size}},
        8192,
    };
    if (!map.try_emplace_back(kernel::mm::Region{
            remote,
            kernel::mm::RegionKind::FirmwareReserved,
        })) {
        return false;
    }
    PmmFixture fixture{primary_memory_storage, libk::move(map)};
    return !fixture
        && fixture.error() == kernel::mm::PmmInitError::InvalidRegion;
}

} // namespace

void register_allocator_tests(TestRegistry& registry) noexcept {
    (void)registry.add("pmm", "owned page destruction releases its frame", test_allocate_owner_releases_on_destruction);
    (void)registry.add("pmm", "move transfers the only release authority", test_owned_page_move_transfers_release_authority);
    (void)registry.add("pmm", "metadata remains outside the free index", test_metadata_is_reserved_and_external_to_free_index);
    (void)registry.add("pmm", "boot reservations require explicit consumption", test_boot_reservation_requires_explicit_consumption);
    (void)registry.add("pmm", "dropped reservation authority can be taken again", test_dropped_reservation_can_be_taken_again);
    (void)registry.add("pmm", "exact boot reservation handoff preserves other reservations", test_exact_boot_reservation_handoff);
    (void)registry.add("pmm", "reservation authority is bound to one owner", test_reservation_authority_is_bound_to_its_owner);
    (void)registry.add("pmm", "discontiguous RAM forms independent arenas", test_discontiguous_ram_forms_multiple_arenas);
    (void)registry.add("pmm", "foreign pages and MMIO stay unmanaged", test_foreign_page_and_mmio_are_not_managed);
    (void)registry.add("pmm", "exhaustion preserves ledger/index equivalence", test_exhaustion_preserves_ledger_index_equivalence);
    (void)registry.add("pmm", "empty page-group move transfers authority", test_empty_page_group_move_transfers_authority);
    (void)registry.add("pmm", "page-group destruction rolls back same-arena pages", test_page_group_rolls_back_same_arena_pages);
    (void)registry.add("pmm", "empty page-group extension releases its borrow", test_empty_page_group_extension_releases_borrow);
    (void)registry.add("pmm", "page-group extension rolls back only its new prefix", test_page_group_extension_rolls_back_only_new_prefix);
    (void)registry.add("pmm", "page-group ownership chains cross arenas", test_page_group_chain_crosses_arenas);
    (void)registry.add("pmm", "page-group detach and reattach preserve one frame owner", test_page_group_detach_and_reattach_preserve_frame);
    (void)registry.add("pmm", "direct map covers proven RAM independent of allocation state", test_direct_map_preserves_ram_independent_of_allocation_state);
    (void)registry.add("pmm", "KernelRoot move transfers architecture ownership", test_page_table_move_transfers_complete_tree);
    (void)registry.add("pmm", "runtime editor separates private and shared table ownership", test_runtime_editor_owns_private_and_shared_tables);
    (void)registry.add("pmm", "Sv39 page-table initialization clears the complete frame", test_sv39_page_table_initialization_clears_complete_frame);
    (void)registry.add("pmm", "Sv39 walk allocates only missing tables", test_sv39_walk_allocates_only_missing_tables);
    (void)registry.add("pmm", "Sv39 mapping failures preserve allocations", test_sv39_mapping_failures_preserve_allocations);
    (void)registry.add("pmm", "Sv39 missing branches roll back partial allocation", test_sv39_missing_branch_rolls_back_partial_allocation);
    (void)registry.add("pmm", "Sv39 range helpers map and inspect contiguous leaves", test_sv39_range_helpers_map_and_inspect);
    (void)registry.add("pmm", "Sv39 range helper keeps mapped prefix on failure", test_sv39_range_helper_keeps_mapped_prefix_on_failure);
    (void)registry.add("pmm", "initial page-table exhaustion rolls back unpublished ownership", test_initial_page_table_exhaustion_rolls_back);
    (void)registry.add("pmm", "direct-map policy rejects unrepresentable RAM", test_initial_page_table_unrepresentable_range_rolls_back);
    (void)registry.add("pmm", "direct-map construction rejects empty maps", test_empty_map_is_rejected);
    (void)registry.add("pmm", "invalid regions are rejected", test_invalid_region_is_rejected);
    (void)registry.add("pmm", "direct-map construction rejects overlapping regions", test_overlapping_regions_are_rejected);
    (void)registry.add("pmm", "available RAM is required", test_available_ram_is_required);
    (void)registry.add("pmm", "direct-map policy rejects RAM outside its window", test_metadata_capacity_failure_is_explicit);
}
