#include <expected>
#include <test/test.hpp>

#include <mm/table.hpp>
#include <mm/kspace.hpp>
#include <csr.hpp>
#include <libk/concepts.hpp>
#include <libk/inplace_vector.hpp>
#include <libk/manual_lifetime.hpp>
#include <libk/noncopyable.hpp>
#include <libk/mem.h>
#include <utility>
#include <mm/pmm.hpp>
#include <mm/tlb.hpp>
#include <boot/link.hpp>
#include <boot/info.hpp>

namespace {


constinit libk::ManualLifetime<mm::Pmm> primary_memory_storage{};
constinit libk::ManualLifetime<mm::Pmm> secondary_memory_storage{};

inline constexpr size_t test_pages = 512;
alignas(mm::page_size) uint8_t test_ram[test_pages * mm::page_size]{};

class PmmFixture : private libk::noncopyable_nonmovable {
  public:
    PmmFixture(
        libk::ManualLifetime<mm::Pmm>& storage,
        mm::RegionList&& memory_map) noexcept
        : storage_(storage),
          initialization_(initialize(std::move(memory_map))) {}

    ~PmmFixture() noexcept {
        storage_.reset();
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(initialization_);
    }

    [[nodiscard]] auto error() const noexcept -> mm::PmmInitError {
        return initialization_.error();
    }

    [[nodiscard]] auto memory(this auto& self) noexcept -> decltype(auto) {
        return *self.storage_;
    }

  private:
    [[nodiscard]] auto initialize(mm::RegionList&& memory_map) noexcept
        -> mm::Pmm::InitializationResult {
        return mm::Pmm::initialize_in(
            storage_, std::move(memory_map), mm::Pmm::Window{
                .pa = mm::Phys{
                    kernel_phys(mm::Virt{
                        reinterpret_cast<uintptr_t>(test_ram)})->raw()},
                .va = mm::Virt{
                    reinterpret_cast<uintptr_t>(test_ram)},
                .size = sizeof(test_ram),
            });
    }

    libk::ManualLifetime<mm::Pmm>& storage_;
    mm::Pmm::InitializationResult initialization_;
};

[[nodiscard]] auto page_at(size_t offset) noexcept -> mm::Page {
    const auto base = kernel_phys(mm::Virt{
        reinterpret_cast<uintptr_t>(test_ram)});
    libk_assert(base);
    const auto address = base->checked_add(offset * mm::page_size);
    libk_assert(address);
    return *mm::Page::from_base(*address);
}

[[nodiscard]] auto page_range(size_t offset, size_t pages) noexcept
    -> mm::Pages {
    return mm::Pages{page_at(offset), pages};
}

[[nodiscard]] auto append_region(
    mm::RegionList& map,
    size_t offset,
    size_t pages,
    mm::Region::Kind kind) noexcept -> bool {
    return map.try_emplace_back(mm::Region{page_range(offset, pages), kind});
}

[[nodiscard]] auto make_available_map(size_t pages = 64) noexcept
    -> mm::RegionList {
    mm::RegionList map{};
    (void)append_region(map, 0, pages, mm::Region::Kind::Ram);
    return map;
}

consteval auto sv39_pte_representation_contract() noexcept -> bool {
    constexpr auto page =
        mm::Page{0x12345};

    constexpr auto invalid_page =
        mm::Page{~uintptr_t{0}};

    const auto non_leaf =
        arch::Pte::non_leaf(page);

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
        arch::Pte::leaf_4k(
            page,
            arch::PtPerm::Rw);
    const auto user_rx =
        arch::Pte::leaf_4k(
            page,
            arch::PtPerm::UserRx);
    const auto user_rw =
        arch::Pte::leaf_4k(
            page,
            arch::PtPerm::UserRw);

    constexpr uint64_t expected_leaf =
        (uint64_t{0x12345} << 10)
        | (uint64_t{1} << 0)
        | (uint64_t{1} << 1)
        | (uint64_t{1} << 2)
        | (uint64_t{1} << 6)
        | (uint64_t{1} << 7);

    const auto leaf_page = leaf
        ? leaf->leaf_page()
        : std::nullopt;

    constexpr uint64_t global_bit = uint64_t{1} << 5;
    constexpr uint64_t rsw_bits = uint64_t{3} << 8;
    constexpr uint64_t reserved_high = uint64_t{1} << 63;
    const auto global_leaf = arch::Pte::from_raw(
        expected_leaf | global_bit);
    const auto rsw_leaf = arch::Pte::from_raw(
        expected_leaf | rsw_bits);
    const auto global_rsw_branch = arch::Pte::from_raw(
        non_leaf->raw() | global_bit | rsw_bits);
    const auto reserved_leaf = arch::Pte::from_raw(
        expected_leaf | reserved_high);
    const auto reserved_branch = arch::Pte::from_raw(
        non_leaf->raw() | reserved_high);
    const auto write_without_read = arch::Pte::from_raw(
        uint64_t{1} | (uint64_t{1} << 2));

    return leaf
        && leaf->raw() == expected_leaf
        && leaf->is_leaf()
        && leaf_page
        && *leaf_page == page
        && leaf->has_permissions(
            arch::PtPerm::Rw)
        && leaf->accessed()
        && leaf->dirty()
        && !leaf->with_usage(false, false).accessed()
        && !leaf->with_usage(false, false).dirty()
        && !leaf->has_permissions(
            arch::PtPerm::Ro)
        && user_rx
        && user_rx->has_permissions(arch::PtPerm::UserRx)
        && user_rx->raw()
            == ((uint64_t{0x12345} << 10)
                | (uint64_t{1} << 0)
                | (uint64_t{1} << 1)
                | (uint64_t{1} << 3)
                | (uint64_t{1} << 4)
                | (uint64_t{1} << 6)
                | (uint64_t{1} << 7))
        && user_rw
        && user_rw->has_permissions(arch::PtPerm::UserRw)
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
        && !arch::Pte::non_leaf(invalid_page)
        && !arch::Pte::leaf_4k(
            invalid_page,
            arch::PtPerm::Rw);
}

static_assert(sv39_pte_representation_contract());

consteval auto satp_representation_contract() noexcept -> bool {
    constexpr usize max_ppn =
        (usize{1} << csr::Satp::PPN_WIDTH) - 1;
    constexpr usize max_asid =
        (usize{1} << csr::Satp::ASID_WIDTH) - 1;
    const auto maximum = csr::Satp::try_make_sv39(
        max_ppn,
        max_asid);
    return maximum
        && csr::Satp::mode(*maximum) == csr::Satp::MODE_SV39
        && csr::Satp::ppn(*maximum) == max_ppn
        && csr::Satp::asid(*maximum) == max_asid
        && !csr::Satp::try_make_sv39(max_ppn + 1)
        && !csr::Satp::try_make_sv39(0, max_asid + 1);
}

static_assert(satp_representation_contract());

bool test_allocate_owner_releases_on_destruction(const TestContext&) noexcept {
    PmmFixture fixture{primary_memory_storage, make_available_map()};
    if (!fixture) {
        return false;
    }
    auto& memory = fixture.memory();
    const size_t initial_free = memory.free_page_count();
    mm::Page page{};

    {
        auto allocation = memory.allocate_page();
        if (!allocation) {
            return false;
        }
        auto owner = std::move(allocation).value();
        page = owner.page();
        const auto state = memory.state_of(page);
        if (!state || state.value() != mm::PageState::Allocated
            || memory.free_page_count() + 1 != initial_free) {
            return false;
        }
    }

    const auto state = memory.state_of(page);
    return state
        && state.value() == mm::PageState::Free
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
    auto first = std::move(allocation).value();
    const mm::Page page = first.page();
    mm::OwnedPage second{std::move(first)};
    if (first || !second || second.page() != page) {
        return false;
    }
    second.reset();
    const auto state = memory.state_of(page);
    return !second && state && state.value() == mm::PageState::Free;
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
        && first.value() == mm::PageState::Reserved
        && stats.arena_count == 1
        && stats.metadata_pages != 0
        && stats.reserved_pages == stats.metadata_pages
        && stats.free_pages + stats.reserved_pages == 64
        && memory.verify_invariants();
}

bool test_boot_reservation_requires_explicit_consumption(const TestContext&) noexcept {
    mm::RegionList map{};
    if (!append_region(map, 0, 48, mm::Region::Kind::Ram)
        || !append_region(map, 48, 3, mm::Region::Kind::Boot)
        || !append_region(map, 51, 13, mm::Region::Kind::Ram)) {
        return false;
    }
    PmmFixture fixture{primary_memory_storage, std::move(map)};
    if (!fixture) {
        return false;
    }
    auto& memory = fixture.memory();
    const mm::Page reclaimed_page = page_at(48);
    const auto before = memory.state_of(reclaimed_page);
    const size_t initial_free = memory.free_page_count();
    auto reservation = memory.take_boot();
    if (!before || before.value() != mm::PageState::Reserved
        || !reservation || reservation->range().page_count() != 3) {
        return false;
    }
    auto reclaimed = memory.reclaim(std::move(*reservation));
    const auto after = memory.state_of(reclaimed_page);
    return reclaimed
        && reclaimed.value() == 3
        && after
        && after.value() == mm::PageState::Free
        && memory.free_page_count() == initial_free + 3
        && !memory.take_boot()
        && memory.verify_invariants();
}

bool test_dropped_reservation_can_be_taken_again(const TestContext&) noexcept {
    mm::RegionList map{};
    if (!append_region(map, 0, 48, mm::Region::Kind::Ram)
        || !append_region(map, 48, 2, mm::Region::Kind::Boot)) {
        return false;
    }
    PmmFixture fixture{primary_memory_storage, std::move(map)};
    if (!fixture) {
        return false;
    }
    auto& memory = fixture.memory();
    {
        auto reservation = memory.take_boot();
        if (!reservation) {
            return false;
        }
    }
    return static_cast<bool>(memory.take_boot());
}

bool test_exact_boot_reservation_handoff(const TestContext&) noexcept {
    mm::RegionList map{};
    if (!append_region(map, 0, 48, mm::Region::Kind::Ram)
        || !append_region(map, 48, 2, mm::Region::Kind::Boot)
        || !append_region(map, 50, 2, mm::Region::Kind::Ram)
        || !append_region(map, 52, 3, mm::Region::Kind::Boot)
        || !append_region(map, 55, 9, mm::Region::Kind::Ram)) {
        return false;
    }
    PmmFixture fixture{primary_memory_storage, std::move(map)};
    if (!fixture) {
        return false;
    }
    auto& memory = fixture.memory();
    const mm::Pages target = page_range(52, 3);
    auto selected = memory.take_boot(target);
    if (!selected
        || selected->range().base() != target.base()
        || selected->range().page_count() != target.page_count()
        || memory.take_boot(page_range(51, 3))) {
        return false;
    }
    auto remaining = memory.take_boot();
    if (!remaining
        || remaining->range().base() != page_at(48)
        || remaining->range().page_count() != 2) {
        return false;
    }
    const auto selected_reclaimed = memory.reclaim(std::move(*selected));
    const auto remaining_reclaimed = memory.reclaim(std::move(*remaining));
    return selected_reclaimed && selected_reclaimed.value() == 3
        && remaining_reclaimed && remaining_reclaimed.value() == 2
        && memory.verify_invariants();
}

bool test_boot_reservation_adopts_owned_pages(const TestContext&) noexcept {
    mm::RegionList map{};
    if (!append_region(map, 0, 48, mm::Region::Kind::Ram)
        || !append_region(
            map, 48, 3, mm::Region::Kind::Boot)
        || !append_region(map, 51, 13, mm::Region::Kind::Ram)) {
        return false;
    }
    PmmFixture fixture{primary_memory_storage, std::move(map)};
    if (!fixture) {
        return false;
    }
    auto& memory = fixture.memory();
    const size_t initial_free = memory.free_page_count();
    auto reservation = memory.take_boot(page_range(48, 3));
    if (!reservation) {
        return false;
    }
    auto adopted = memory.adopt(std::move(*reservation));
    if (!adopted) {
        return false;
    }
    auto pages = std::move(adopted).value();
    const auto state = memory.state_of(page_at(49));
    if (!state || state.value() != mm::PageState::Allocated
        || pages.page_count() != 3 || !pages.contains(page_at(48))
        || !pages.contains(page_at(50))
        || memory.free_page_count() != initial_free
        || memory.take_boot(page_range(48, 3))) {
        return false;
    }
    pages.reset();
    return memory.free_page_count() == initial_free + 3
        && memory.verify_invariants();
}

bool test_reservation_authority_is_bound_to_its_owner(const TestContext&) noexcept {
    mm::RegionList first_map{};
    mm::RegionList second_map{};
    if (!append_region(first_map, 0, 48, mm::Region::Kind::Ram)
        || !append_region(first_map, 48, 2, mm::Region::Kind::Boot)
        || !append_region(first_map, 50, 14, mm::Region::Kind::Ram)
        || !append_region(second_map, 96, 64, mm::Region::Kind::Ram)) {
        return false;
    }
    PmmFixture first_fixture{
        primary_memory_storage, std::move(first_map)};
    if (!first_fixture) {
        return false;
    }
    PmmFixture second_fixture{
        secondary_memory_storage, std::move(second_map)};
    if (!second_fixture) {
        return false;
    }
    auto& first = first_fixture.memory();
    auto& second = second_fixture.memory();
    auto reservation = first.take_boot();
    if (!reservation) {
        return false;
    }
    const auto reclaimed = second.reclaim(std::move(*reservation));
    return !reclaimed
        && reclaimed.error() == mm::BootErr::WrongOwner
        && first.verify_invariants()
        && second.verify_invariants();
}

bool test_discontiguous_ram_forms_multiple_arenas(const TestContext&) noexcept {
    mm::RegionList map{};
    if (!append_region(map, 96, 32, mm::Region::Kind::Ram)
        || !append_region(map, 0, 64, mm::Region::Kind::Ram)) {
        return false;
    }
    PmmFixture fixture{primary_memory_storage, std::move(map)};
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
    mm::RegionList map = make_available_map();
    if (!append_region(map, 96, 2, mm::Region::Kind::Mmio)) {
        return false;
    }
    PmmFixture fixture{primary_memory_storage, std::move(map)};
    if (!fixture) {
        return false;
    }
    auto& memory = fixture.memory();
    const mm::Page foreign = page_at(80);
    const mm::Page mmio = page_at(96);
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
    libk::InplaceVector<mm::OwnedPage, 32> owners{};
    const size_t available = memory.free_page_count();
    for (size_t index = 0; index < available; ++index) {
        auto allocation = memory.allocate_page();
        if (!allocation
            || !owners.try_push_back(std::move(allocation).value())
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

    auto first = memory.group();
    mm::PageGroup second{std::move(first)};
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
    mm::Page pages[3]{};

    {
        auto group = memory.group();
        {
            auto pending = group.owner().group();
            for (size_t index = 0; index < 3; ++index) {
                auto allocation = pending.allocate();
                if (!allocation) {
                    return false;
                }
                pages[index] = allocation.value();
            }
            group.append(std::move(pending));
        }
        if (group.page_count() != 3
            || memory.free_page_count() + 3 != initial_free
            || !memory.verify_invariants()) {
            return false;
        }
    }

    for (const mm::Page page : pages) {
        const auto state = memory.state_of(page);
        if (!state || state.value() != mm::PageState::Free) {
            return false;
        }
    }
    return memory.free_page_count() == initial_free
        && memory.verify_invariants();
}

bool test_empty_page_group_rollback(
    const TestContext&) noexcept {
    PmmFixture fixture{
        primary_memory_storage,
        make_available_map()};
    if (!fixture) {
        return false;
    }

    auto& memory = fixture.memory();
    const size_t initial_free = memory.free_page_count();
    auto group = memory.group();

    {
        auto pending = group.owner().group();
    }

    {
        auto pending = group.owner().group();
        auto allocation = pending.allocate();
        if (!allocation) {
            return false;
        }
        group.append(std::move(pending));
    }

    return group.page_count() == 1
        && memory.free_page_count() + 1 == initial_free
        && memory.verify_invariants();
}

bool test_page_group_prepare_rollback(
    const TestContext&) noexcept {
    PmmFixture fixture{
        primary_memory_storage,
        make_available_map()};
    if (!fixture) {
        return false;
    }

    auto& memory = fixture.memory();
    const size_t initial_free = memory.free_page_count();
    auto group = memory.group();
    mm::Page retained{};
    mm::Page rolled_back[3]{};

    {
        auto pending = group.owner().group();
        auto allocation = pending.allocate();
        if (!allocation) {
            return false;
        }
        retained = allocation.value();
        group.append(std::move(pending));
    }

    const size_t committed_free = memory.free_page_count();
    {
        auto pending = group.owner().group();
        for (size_t index = 0; index < 3; ++index) {
            auto allocation = pending.allocate();
            if (!allocation) {
                return false;
            }
            rolled_back[index] = allocation.value();
        }

        if (group.page_count() != 1
            || pending.page_count() != 3
            || memory.free_page_count() + 3 != committed_free
            || !memory.verify_invariants()) {
            return false;
        }
    }

    const auto retained_state = memory.state_of(retained);
    if (group.page_count() != 1
        || memory.free_page_count() != committed_free
        || !retained_state
        || retained_state.value() != mm::PageState::Allocated) {
        return false;
    }

    for (const mm::Page page : rolled_back) {
        const auto state = memory.state_of(page);
        if (!state || state.value() != mm::PageState::Free) {
            return false;
        }
    }

    group.reset();
    return memory.free_page_count() == initial_free
        && memory.verify_invariants();
}

bool test_page_group_chain_crosses_arenas(const TestContext&) noexcept {
    mm::RegionList map{};
    const auto first_range = page_range(0, 16);
    const auto second_range = page_range(32, 16);
    if (!map.try_emplace_back(mm::Region{
            first_range, mm::Region::Kind::Ram})
        || !map.try_emplace_back(mm::Region{
            second_range, mm::Region::Kind::Ram})) {
        return false;
    }

    PmmFixture fixture{primary_memory_storage, std::move(map)};
    if (!fixture) {
        return false;
    }
    auto& memory = fixture.memory();
    const size_t initial_free = memory.free_page_count();
    bool saw_first = false;
    bool saw_second = false;

    {
        auto group = memory.group();
        {
            auto pending = group.owner().group();
            for (size_t index = 0; index < initial_free; ++index) {
                auto allocation = pending.allocate();
                if (!allocation) {
                    return false;
                }
                saw_first = saw_first
                    || first_range.contains(allocation.value());
                saw_second = saw_second
                    || second_range.contains(allocation.value());
            }
            if (!saw_first || !saw_second
                || pending.page_count() != initial_free
                || pending.allocate()
                || !memory.verify_invariants()) {
                return false;
            }
            group.append(std::move(pending));
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
        auto group = memory.group();
        auto pending = group.owner().group();
        const auto first = pending.allocate();
        const auto second = pending.allocate();
        if (!first || !second) {
            return false;
        }
        group.append(std::move(pending));
        auto detached = group.detach(first.value());
        if (!detached
            || detached->page() != first.value()
            || group.page_count() != 1
            || memory.free_page_count() + 2 != initial_free
            || !group.attach(std::move(*detached))
            || *detached
            || group.page_count() != 2
            || !memory.verify_invariants()) {
            return false;
        }
    }
    return memory.free_page_count() == initial_free
        && memory.verify_invariants();
}

bool test_direct_map_preserves_ram_independent_of_allocation_state(
    const TestContext&) noexcept {
    mm::RegionList map{};
    const auto available = page_range(0, 48);
    const auto kernel_image = page_range(48, 4);
    const auto firmware = page_range(52, 4);
    const auto reclaimable = page_range(56, 4);
    const auto available_tail = page_range(60, 20);

    if (!map.try_emplace_back(mm::Region{
            available, mm::Region::Kind::Ram})
        || !map.try_emplace_back(mm::Region{
            kernel_image, mm::Region::Kind::Kernel})
        || !map.try_emplace_back(mm::Region{
            firmware, mm::Region::Kind::Firmware})
        || !map.try_emplace_back(mm::Region{
            reclaimable,
            mm::Region::Kind::Boot})
        || !map.try_emplace_back(mm::Region{
            available_tail,
            mm::Region::Kind::Ram})) {
        return false;
    }

    PmmFixture fixture{primary_memory_storage, std::move(map)};
    if (!fixture) {
        return false;
    }
    auto& memory = fixture.memory();
    const auto expected_ram = page_range(0, 80);

    auto coverage = [&] {
        return memory.virt(expected_ram.base().base(),
                   expected_ram.page_count() * mm::page_size).has_value()
            && memory.virt(kernel_image.base().base(), mm::page_size).has_value()
            && memory.virt(firmware.base().base(), mm::page_size).has_value();
    };
    if (memory.arena_count() != 1 || memory.metadata_page_count() == 0 || !coverage())
        return false;

    const auto metadata_state = memory.state_of(available.base());
    if (!metadata_state
        || metadata_state.value()
            != mm::PageState::Reserved) {
        return false;
    }

    auto reservation = memory.take_boot();
    if (!reservation || reservation->range().base()
            != reclaimable.base()) {
        return false;
    }
    const auto reclaimed = memory.reclaim(std::move(*reservation));

    return reclaimed
        && reclaimed.value() == reclaimable.page_count()
        && coverage()
        && memory.verify_invariants();
}

bool test_tables_share_reserve_and_retire(const TestContext&) noexcept {
    PmmFixture fixture{primary_memory_storage, make_available_map(384)};
    if (!fixture) return false;
    auto& pmm = fixture.memory();
    const auto free = pmm.free_page_count();
    {
        auto kernel = mm::PageTable::create(pmm, mm::PageTable::Kind::Kernel);
        if (!kernel || kernel->page_count() != 257) return false;
        auto user = mm::PageTable::create(pmm, mm::PageTable::Kind::User, &*kernel);
        auto payload = pmm.allocate_page();
        if (!user || !payload || user->page_count() != 1) return false;
        auto first = mm::VPage::from_base(mm::Virt{mm::UserBegin});
        auto next = first->checked_add(1);
        auto distant = first->checked_add(512);
        auto count = user->count();
        if (!count.include(*first) || !count.include(*next) || !count.include(*distant)
            || count.pages() != 3 || count.include(*first)) return false;
        auto reserve = pmm.group();
        if (!reserve.grow(count.pages())) return false;
        for (auto va : {*first, *next, *distant})
            if (!user->map(va, payload->page(), arch::PtPerm::UserRw, reserve)) return false;
        if (reserve.page_count() != 0 || user->page_count() != 4) return false;
        // A failed fresh batch preserves old leaves and refunds its new branches.
        auto fresh = *first->checked_add(1024);
        const auto available = pmm.free_page_count();
        usize supplied{};
        auto failed = user->map(fresh, 2, [&] {
            return supplied++ == 0 ? payload->page() : mm::Page{};
        }, arch::PtPerm::UserRw);
        if (failed || failed.error() != mm::PtErr::BadPhys || user->page_count() != 4
            || pmm.free_page_count() != available || user->query(fresh)
            || !user->query(*first) || !user->query(*distant)) return false;
        auto usage = user->usage(*first);
        if (!usage || usage->accessed || usage->dirty) return false;
        // Simulate the same atomic A/D updates as the hardware walker.
        auto* root = reinterpret_cast<arch::Pte*>(pmm.bytes(user->page()));
        auto* middle = reinterpret_cast<arch::Pte*>(pmm.bytes(*root[0].load().next_table_page()));
        auto* leaves = reinterpret_cast<arch::Pte*>(pmm.bytes(*middle[0].load().next_table_page()));
        auto& leaf = leaves[first->raw() & 511];
        leaf.update([](arch::Pte p) noexcept { return p.with_usage(true, true); });
        auto old = user->protect(*first, arch::PtPerm::UserRx);
        auto used = user->clear_usage(*first);
        auto kept = user->usage(*first);
        if (!old || !old->dirty || !used || !used->dirty
            || !kept || kept->accessed || kept->dirty) return false;
        mm::Flush retired{pmm};
        for (auto va : {*first, *next, *distant}) {
            auto removed = user->unmap(va);
            if (!removed) return false;
            for (auto& page : removed->tables)
                if (!retired.adopt(std::move(page))) return false;
        }
        if (user->page_count() != 1 || retired.page_count() != 3 || user->query(*first)) return false;
        resource::Charge refund{};
        if (!retired.release(refund)) return false;
        auto high = mm::VPage::from_base(mm::Virt{mm::DirectBegin});
        auto kernel_count = kernel->count();
        if (!kernel_count.include(*high) || !reserve.grow(kernel_count.pages())
            || !kernel->map(*high, payload->page(), arch::PtPerm::Rw, reserve)) return false;
        auto removed = kernel->unmap(*high);
        if (!removed || removed->tables.size() != 1 || kernel->page_count() != 257) return false;
        // The borrowed high-half branch stays stable after descendant retirement.
        if (root[256].load().next_table_page() != reinterpret_cast<arch::Pte*>(pmm.bytes(kernel->page()))[256].load().next_table_page()) return false;
        const auto token = kernel->cpu_root();
        mm::PageTable moved{std::move(*kernel)};
        if (*kernel || moved.cpu_root() != token) return false;
    }
    return pmm.free_page_count() == free && pmm.verify_invariants();
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
    const auto result = kernel_root(memory);

    return !result
        && result.error()
            == mm::PtErr::NoMemory
        && memory.free_page_count() == initial_free
        && memory.verify_invariants();
}

bool test_initial_page_table_unrepresentable_range_rolls_back(
    const TestContext&) noexcept {
    constexpr auto unrepresentable_range = mm::Pages{
        mm::Page{uintptr_t{1} << 44},
        1,
    };

    auto map = make_available_map();
    if (!map.try_emplace_back(mm::Region{
            unrepresentable_range,
            mm::Region::Kind::Ram})) {
        return false;
    }
    const auto base = kernel_phys(mm::Virt{
        reinterpret_cast<uintptr_t>(test_ram)});
    libk_assert(base);
    const auto result = mm::Pmm::initialize_in(
        secondary_memory_storage, std::move(map),
        mm::Pmm::Window{
            .pa = *base,
            .va = mm::Virt{reinterpret_cast<uintptr_t>(test_ram)},
            .size = sizeof(test_ram),
        });
    secondary_memory_storage.reset();
    return !result && result.error() == mm::PmmInitError::OutsideWindow;
}

bool test_empty_map_is_rejected(const TestContext&) noexcept {
    mm::RegionList map{};
    PmmFixture fixture{primary_memory_storage, std::move(map)};
    return !fixture
        && fixture.error() == mm::PmmInitError::EmptyMemoryMap;
}

bool test_invalid_region_is_rejected(const TestContext&) noexcept {
    {
        mm::RegionList map{};
        (void)append_region(map, 0, 0, mm::Region::Kind::Ram);
        PmmFixture fixture{primary_memory_storage, std::move(map)};
        if (fixture || fixture.error() != mm::PmmInitError::InvalidRegion) return false;
    }
    return true;
}

bool test_overlapping_regions_are_rejected(const TestContext&) noexcept {
    auto map = make_available_map();
    if (!append_region(map, 1, 2, mm::Region::Kind::Boot)) {
        return false;
    }
    PmmFixture fixture{primary_memory_storage, std::move(map)};
    return !fixture
        && fixture.error() == mm::PmmInitError::OverlappingRegions;
}

bool test_available_ram_is_required(const TestContext&) noexcept {
    mm::RegionList map{};
    if (!append_region(map, 0, 2, mm::Region::Kind::Kernel)) {
        return false;
    }
    PmmFixture fixture{primary_memory_storage, std::move(map)};
    return !fixture
        && fixture.error() == mm::PmmInitError::NoRam;
}

bool test_metadata_capacity_failure_is_explicit(const TestContext&) noexcept {
    mm::RegionList map{};
    if (!append_region(map, 0, 1, mm::Region::Kind::Ram)) {
        return false;
    }
    const auto remote = mm::Pages{
        mm::Page{0x90000000 / mm::page_size},
        8192,
    };
    if (!map.try_emplace_back(mm::Region{
            remote,
            mm::Region::Kind::Firmware,
        })) {
        return false;
    }
    PmmFixture fixture{primary_memory_storage, std::move(map)};
    return !fixture
        && fixture.error() == mm::PmmInitError::OutsideWindow;
}

bool test_node_key_survives_page_reuse(const TestContext&) noexcept {
    PmmFixture fixture{primary_memory_storage, make_available_map()};
    if (!fixture) return false;
    auto& memory = fixture.memory();
    const auto free = memory.free_page_count();
    mm::Slab<unsigned> nodes{memory, {.nodes = 1, .pages = 1}};
    auto first = nodes.create({}, 1u);
    if (!first) return false;
    const auto key = first->key;
    auto moved = std::move(nodes);
    const bool retained = !nodes.live_count() && !nodes.find(key)
        && moved.find(key) == first->object;
    moved.destroy(*first->object);
    const bool returned = memory.free_page_count() == free;
    auto second = moved.create({}, 2u);
    if (!second) return false;
    const bool valid = retained && returned && second->key.generation > key.generation
        && moved.find(key) == nullptr && *moved.find(second->key) == 2u;
    moved.destroy(*second->object);
    return valid && memory.free_page_count() == free;

}

} // namespace

void register_allocator_tests(TestRegistry& registry) noexcept {
    (void)registry.add("pmm", "page tables share, reserve and retire actual frames", test_tables_share_reserve_and_retire);
    (void)registry.add("slab", "returning a page cannot resurrect an old node key", test_node_key_survives_page_reuse);
    (void)registry.add("pmm", "owned page destruction releases its frame", test_allocate_owner_releases_on_destruction);
    (void)registry.add("pmm", "move transfers the only release authority", test_owned_page_move_transfers_release_authority);
    (void)registry.add("pmm", "metadata remains outside the free index", test_metadata_is_reserved_and_external_to_free_index);
    (void)registry.add("pmm", "boot reservations require explicit consumption", test_boot_reservation_requires_explicit_consumption);
    (void)registry.add("pmm", "dropped reservation authority can be taken again", test_dropped_reservation_can_be_taken_again);
    (void)registry.add("pmm", "exact boot reservation handoff preserves other reservations", test_exact_boot_reservation_handoff);
    (void)registry.add("pmm", "boot reservation adoption transfers frame ownership", test_boot_reservation_adopts_owned_pages);
    (void)registry.add("pmm", "reservation authority is bound to one owner", test_reservation_authority_is_bound_to_its_owner);
    (void)registry.add("pmm", "discontiguous RAM forms independent arenas", test_discontiguous_ram_forms_multiple_arenas);
    (void)registry.add("pmm", "foreign pages and MMIO stay unmanaged", test_foreign_page_and_mmio_are_not_managed);
    (void)registry.add("pmm", "exhaustion preserves ledger/index equivalence", test_exhaustion_preserves_ledger_index_equivalence);
    (void)registry.add("pmm", "empty page-group move transfers authority", test_empty_page_group_move_transfers_authority);
    (void)registry.add("pmm", "page-group destruction rolls back same-arena pages", test_page_group_rolls_back_same_arena_pages);
    (void)registry.add("pmm", "empty temporary page group releases ownership", test_empty_page_group_rollback);
    (void)registry.add("pmm", "temporary page group rolls back without altering destination", test_page_group_prepare_rollback);
    (void)registry.add("pmm", "page-group ownership chains cross arenas", test_page_group_chain_crosses_arenas);
    (void)registry.add("pmm", "page-group detach and reattach preserve one frame owner", test_page_group_detach_and_reattach_preserve_frame);
    (void)registry.add("pmm", "direct map covers proven RAM independent of allocation state", test_direct_map_preserves_ram_independent_of_allocation_state);
    (void)registry.add("pmm", "initial page-table exhaustion rolls back unpublished ownership", test_initial_page_table_exhaustion_rolls_back);
    (void)registry.add("pmm", "direct-map policy rejects unrepresentable RAM", test_initial_page_table_unrepresentable_range_rolls_back);
    (void)registry.add("pmm", "PMM rejects empty layouts", test_empty_map_is_rejected);
    (void)registry.add("pmm", "invalid regions are rejected", test_invalid_region_is_rejected);
    (void)registry.add("pmm", "PMM rejects overlapping regions", test_overlapping_regions_are_rejected);
    (void)registry.add("pmm", "available RAM is required", test_available_ram_is_required);
    (void)registry.add("pmm", "direct-map policy rejects RAM outside its window", test_metadata_capacity_failure_is_explicit);
}
