#include <test/test.hpp>
#include <mm/table.hpp>

#include <libk/manual_lifetime.hpp>
#include <libk/noncopyable.hpp>
#include <libk/scope_guard.hpp>
#include <libk/span.hpp>
#include <utility>
#include <mm/mem.hpp>
#include <mm/pager.hpp>
#include <object/pool.hpp>
#include <object/group.hpp>
#include <mm/vspace.hpp>
#include <boot/info.hpp>

namespace {

constexpr usize memory_test_pages = 160;
constexpr usize reserved_pages = 8;
alignas(mm::page_size) byte
    memory_test_ram[memory_test_pages * mm::page_size]{};
constinit libk::ManualLifetime<mm::RegionList> memory_test_map{};
constinit libk::ManualLifetime<mm::Pmm> memory_test_pmm{};
constinit libk::ManualLifetime<WorkQueue> memory_work{};
constinit libk::ManualLifetime<object::store<mm::VSpace, mm::Mem, Pager>> memory_test_mm{};

constinit libk::ManualLifetime<mm::Mem> memory_test_object{};
constinit libk::ManualLifetime<mm::Mem> memory_test_peer{};
constinit libk::ManualLifetime<mm::Mem> memory_test_staging{};

struct StagingReset final {
    ~StagingReset() noexcept {
            if (memory_test_staging) {
            memory_test_staging->retire();
            memory_test_staging.reset();
        }
    }
};

[[nodiscard]] auto page_at(usize offset) noexcept -> mm::Page {
    const auto physical = boot_layout.phys(mm::Virt{
        reinterpret_cast<usize>(memory_test_ram)});
    libk_assert(physical);
    const auto address = physical->checked_add(offset * mm::page_size);
    libk_assert(address);
    const auto page = mm::Page::from_base(*address);
    libk_assert(page);
    return *page;
}

class MemoryFixture final : private libk::noncopyable_nonmovable {
public:
    MemoryFixture() noexcept = default;
    ~MemoryFixture() noexcept { reset(); }

    [[nodiscard]] auto initialize() noexcept -> bool {
        reset();
        (void)memory_work.emplace();
        const auto physical = boot_layout.phys(mm::Virt{
            reinterpret_cast<usize>(memory_test_ram)});
        if (!physical) {
            return false;
        }
        auto& map = memory_test_map.emplace();
        libk_assert(map.try_emplace_back(mm::Region{
            {*mm::Page::from_base(mm::Phys{0x1000'0000}), 1}, mm::Region::Kind::Mmio}));
        if (!map.try_emplace_back(mm::Region{
                mm::Pages{page_at(0), reserved_pages},
                mm::Region::Kind::Kernel})
            || !map.try_emplace_back(mm::Region{
                mm::Pages{
                    page_at(reserved_pages),
                    memory_test_pages - reserved_pages},
                mm::Region::Kind::Ram})) {
            reset();
            return false;
        }
        if (!mm::Pmm::initialize_in(
                memory_test_pmm, std::move(map), mm::Pmm::Window{
                .pa = *physical,
                .va = mm::Virt{
                    reinterpret_cast<usize>(memory_test_ram)},
                .size = sizeof(memory_test_ram),
            })) {
            reset();
            return false;
        }
        memory_test_map.reset();

        [[maybe_unused]] auto& memory =
            memory_test_mm.emplace(*memory_test_pmm, *memory_work);
        return true;
    }

    [[nodiscard]] auto make(usize bytes, mm::Mem::Config cfg) noexcept
        -> std::expected<mm::Mem*, mm::MemErr> {
        if (memory_test_object) { memory_test_object->retire(); memory_test_object.reset(); }
        auto data = mm::Mem::prepare({}, *memory_test_pmm, bytes, std::move(cfg));
        if (!data) return std::unexpected(data.error());
        return &memory_test_object.emplace(std::move(*data));
    }

    [[nodiscard]] auto make_peer(usize bytes, mm::Mem::Config cfg) noexcept
        -> std::expected<mm::Mem*, mm::MemErr> {
        if (memory_test_peer) { memory_test_peer->retire(); memory_test_peer.reset(); }
        auto data = mm::Mem::prepare({}, *memory_test_pmm, bytes, std::move(cfg));
        if (!data) return std::unexpected(data.error());
        return &memory_test_peer.emplace(std::move(*data));
    }

    [[nodiscard]] auto pmm() noexcept -> mm::Pmm& {
        return *memory_test_pmm;
    }

    auto memory() noexcept -> object::store<mm::VSpace, mm::Mem, Pager>& { return *memory_test_mm; }

    [[nodiscard]] auto make_pager() noexcept -> bool {
        if (pager_) {
            return false;
        }
        auto pending = memory().get<Pager>().create();
        if (!pending) {
            return false;
        }
        pager_ = std::move(pending).value().publish();
        return static_cast<bool>(pager_);
    }

    [[nodiscard]] auto make_wrong_pager() noexcept -> bool {
        if (wrong_pager_) {
            return false;
        }
        auto pending = memory().get<Pager>().create();
        if (!pending) {
            return false;
        }
        wrong_pager_ = std::move(pending).value().publish();
        return static_cast<bool>(wrong_pager_);
    }

    [[nodiscard]] auto pager() noexcept -> Pager& {
        libk_assert(pager_);
        return pager_.get();
    }

    [[nodiscard]] auto pager_ref() noexcept {
        libk_assert(pager_);
        return pager_.erase();
    }

    [[nodiscard]] auto wrong_pager() noexcept -> Pager& {
        libk_assert(wrong_pager_);
        return wrong_pager_.get();
    }

    void keep(object::ref<mm::Mem>&& memory) noexcept {
        libk_assert(!pooled_);
        pooled_ = std::move(memory);
    }

    [[nodiscard]] auto pooled() noexcept
        -> object::ref<mm::Mem>& {
        return pooled_;
    }

    void release_pooled() noexcept { pooled_.reset(); }

private:
    void reset() noexcept {
        if (memory_test_object) {
            memory_test_object->retire();
            memory_test_object.reset();
        }
        if (memory_test_peer) {
            memory_test_peer->retire();
            memory_test_peer.reset();
        }
        if (pooled_) {
            (void)pooled_.retire();
            pooled_.reset();
        }
        if (wrong_pager_) {
            (void)wrong_pager_.retire();
            wrong_pager_.reset();
        }
        if (pager_) {
            (void)pager_.retire();
            pager_.reset();
        }
        if (memory_test_mm) {
            while (memory_work->run()) {}
        }
        memory_test_mm.reset();

        memory_work.reset();
        memory_test_pmm.reset();
        memory_test_map.reset();
    }

    object::ref<mm::Mem> pooled_{};
    object::ref<Pager> pager_{};
    object::ref<Pager> wrong_pager_{};
};

struct FakeMapping final : private libk::noncopyable_nonmovable {
    FakeMapping() noexcept : attachment(this, ops) {}

    static void invalidate(
        void* context,
        mm::MemWork&& work) noexcept {
        auto& self = *static_cast<FakeMapping*>(context);
        ++self.invalidations;
        self.work = std::move(work);
    }

    static void released(void* context) noexcept {
        ++static_cast<FakeMapping*>(context)->releases;
    }

    inline static const mm::MemOps ops{
        invalidate,
        released,
    };

    mm::MemLink attachment;
    mm::MemWork work{};
    usize invalidations{};
    usize releases{};
};

bool test_io_root_isolated_range_and_refund(const TestContext&) noexcept {
    MemoryFixture fixture{};
    if (!fixture.initialize()) return false;
    auto backing = fixture.pmm().allocate_page();
    if (!backing) return false;
    const mm::Page pages[] = {backing.value().page(), backing.value().page()};
    const usize free = fixture.pmm().free_page_count();
    {
        auto io = mm::PageTable::dma(fixture.pmm(), 0x1ff000, pages, true);
        if (!io || io.value().page_count() != 4) return false;
        const auto& pmm = fixture.pmm();
        auto root = pmm.ptr<const u64>(io.value().page().base(), 512);
        if (!root || (root.value()[0] & 0x3ff) != 1) return false;
        for (usize index = 1; index < 512; ++index)
            if (root.value()[index] != 0) return false;
        auto middle = pmm.ptr<const u64>(
            mm::Phys{(root.value()[0] >> 10) << 12}, 512);
        if (!middle) return false;
        for (usize index = 0; index < 512; ++index) {
            if (index >= 2) {
                if (middle.value()[index] != 0) return false;
                continue;
            }
            if ((middle.value()[index] & 0x3ff) != 1) return false;
            auto leaves = pmm.ptr<const u64>(
                mm::Phys{(middle.value()[index] >> 10) << 12}, 512);
            if (!leaves) return false;
            for (usize leaf = 0; leaf < 512; ++leaf) {
                const u64 expected = leaf == (index == 0 ? 511 : 0)
                    ? (u64{backing.value().page().raw()} << 10) | 0xd7
                    : 0;
                if (leaves.value()[leaf] != expected) return false;
            }
        }
    }
    if (fixture.pmm().free_page_count() != free || !backing.value()) return false;
    // Cross both table levels using a sequential source, as IOSpace does
    // when its PageHolds occupy several metadata pages.
    usize consumed{};
    auto next = [&]() noexcept {
        ++consumed;
        return backing.value().page();
    };
    {
        const auto required = mm::PageTable::dma_pages(0x3ffff000, 2);
        auto io = mm::PageTable::dma(fixture.pmm(), 0x3ffff000, 2,
            next, false);
        if (!required || required.value() != 5 || !io
            || io.value().page_count() != required.value() || consumed != 2)
            return false;
    }
    const usize invalid_first[] = {0, 1, usize{1} << 38};
    for (const usize first : invalid_first) {
        auto io = mm::PageTable::dma(fixture.pmm(), first, 1,
            next, false);
        if (io || consumed != 2) return false;
    }
    if (mm::PageTable::dma_pages(0x1000, 0)
        || mm::PageTable::dma_pages((usize{1} << 38) - 4096, 2)
        || mm::PageTable::dma_pages(0x1000, ~usize{0})) return false;
    const mm::Page invalid[] = {
        mm::Page{usize{1} << 44}};
    auto rejected = mm::PageTable::dma(fixture.pmm(), 0x1000, invalid, false);
    return !rejected && rejected.error() == mm::PtErr::BadPhys
        && fixture.pmm().free_page_count() == free;
}

bool test_anonymous_sparse_pages_own_zeroed_frames(
    const TestContext&) noexcept {
    MemoryFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }
    const usize free_before = fixture.pmm().free_page_count();

    auto memory_ready = fixture.make(8 * mm::page_size, mm::AnonCfg{});
    if (!memory_ready) return false;
    auto& memory = **memory_ready;
    if (memory.kind() != mm::BackingKind::Anonymous
        || memory.query(5).value() != mm::ContentState::Zero) {
        return false;
    }

    mm::Page resident{};
    {
        auto materialized = memory.materialize(5);
        if (!materialized) {
            return false;
        }
        auto lease = std::move(materialized).value();
        resident = lease.page();
        const byte* const bytes = fixture.pmm().bytes(resident);
        if (bytes[0] != 0 || bytes[mm::page_size - 1] != 0
            || !lease.perms().contains(mm::Perm::Write)
            || !fixture.pmm().is_ram(lease.page())) {
            return false;
        }
        fixture.pmm().bytes(resident)[37] = byte{0x5a};
    }
    {
        auto materialized = memory.materialize(5);
        if (!materialized
            || materialized.value().page() != resident
            || fixture.pmm().bytes(resident)[37] != byte{0x5a}
            || memory.query(3).value() != mm::ContentState::Zero) {
            return false;
        }
        auto lease = std::move(materialized).value();
        memory.retire();
        if (memory.state() != mm::MemState::Stopping
            || fixture.pmm().state_of(resident).value()
                != mm::PageState::Allocated) {
            return false;
        }
    }
    const bool lazy_complete = memory.state() == mm::MemState::Retired
        && fixture.pmm().state_of(resident).value() == mm::PageState::Free
        && fixture.pmm().free_page_count() == free_before
        && fixture.pmm().verify_invariants();
    if (!lazy_complete) {
        return false;
    }


    auto eager_ready = fixture.make(3 * mm::page_size, mm::AnonCfg{
            .perms = mm::Perms::of(mm::Perm::Read, mm::Perm::Write),
            .eager = true,
        });
    if (!eager_ready) return false;
    auto& eager = **eager_ready;
    if (!eager_ready) {
        return false;
    }
    for (usize index = 0; index < eager.page_count(); ++index) {
        auto state = eager.query(index);
        if (!state || state.value() != mm::ContentState::Resident) {
            return false;
        }
    }
    eager.retire();
    return eager.state() == mm::MemState::Retired
        && fixture.pmm().free_page_count() == free_before;
}

bool test_physical_backing_borrows_reserved_and_device_extents(
    const TestContext&) noexcept {
    MemoryFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }
    const usize free_before = fixture.pmm().free_page_count();
    constexpr auto read_execute = mm::Perms::of(
        mm::Perm::Read, mm::Perm::Execute);
    constexpr auto read_write = mm::Perms::of(
        mm::Perm::Read, mm::Perm::Write);
    const mm::Page device = *mm::Page::from_base(mm::Phys{0x1000'0000});
    const mm::Extent extents[]{
        {
            .object = {0, 2},
            .physical = {page_at(0), 2},
            .perms = read_execute,
        },
        {
            .object = {2, 1},
            .physical = {device, 1},
            .perms = read_write,
        },
    };

    auto memory_ready = fixture.make(3 * mm::page_size, mm::PhysCfg{libk::Span<const mm::Extent>{extents}, {}});
    if (!memory_ready) return false;
    auto& memory = **memory_ready;
    {
        auto code = memory.materialize(1);
        auto mmio = memory.materialize(2);
        if (!code || !mmio
            || code.value().page() != page_at(1)
            || code.value().perms() != read_execute
            || mmio.value().page() != device
            || fixture.pmm().is_ram(mmio.value().page())) {
            return false;
        }
    }
    byte output{};
    const auto device_read = memory.read(2 * mm::page_size, {&output, 1});
    if (device_read || device_read.error() != mm::MemErr::NotRam) return false;
    memory.retire();
    if (fixture.pmm().state_of(page_at(0)).value()
            != mm::PageState::Reserved
        || fixture.pmm().free_page_count() != free_before) {
        return false;
    }


    const mm::Extent free_extent[]{
        {
            .object = {0, 1},
            .physical = {page_at(reserved_pages + 20), 1},
            .perms = read_write,
        },
    };
    const auto rejected = fixture.make(mm::page_size, mm::PhysCfg{libk::Span<const mm::Extent>{free_extent}, {}});
    if (rejected
        || rejected.error() != mm::MemErr::OwnershipMismatch) {
        return false;
    }


    const mm::Extent conflicting_extents[]{
        {
            .object = {0, 1},
            .physical = {device, 1},
            .perms = read_write,
        },
        {
            .object = {1, 1},
            .physical = {device, 1},
            .perms = read_write,
        },
    };
    const auto alias = fixture.make(2 * mm::page_size, mm::PhysCfg{libk::Span<const mm::Extent>{conflicting_extents}, {}});
    if (alias || alias.error() != mm::MemErr::InvalidRange) return false;
    // External physical memory must come from the inventory, not merely
    // lie outside allocator-owned RAM. An arbitrary bus address has no owner.

    const mm::Extent hole{{0, 1}, {mm::Page{0x20000000 / mm::page_size}, 1}, read_write};
    auto rejected_hole = fixture.make(mm::page_size, mm::PhysCfg{{&hole, 1}, {}});
    return !rejected_hole && rejected_hole.error() == mm::MemErr::NotBacked;
}

bool test_boot_image_distinguishes_borrowed_and_owned_frames(
    const TestContext&) noexcept {
    MemoryFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }
    constexpr auto read_only = mm::Perms::of(mm::Perm::Read);
    const mm::Extent borrowed_extent[]{
        {
            .object = {0, 1},
            .physical = {page_at(2), 1},
            .perms = read_only,
        },
    };

    auto borrowed_ready = fixture.make(mm::page_size, mm::PhysCfg{libk::Span<const mm::Extent>{borrowed_extent}, {}});
    if (!borrowed_ready) return false;
    auto& borrowed = **borrowed_ready;
    if (!borrowed_ready) {
        return false;
    }
    borrowed.retire();
    if (fixture.pmm().state_of(page_at(2)).value()
        != mm::PageState::Reserved) {
        return false;
    }
    memory_test_object.reset();

    const usize free_before = fixture.pmm().free_page_count();
    auto owned = fixture.pmm().group();
    mm::Page pages[2]{};
    {
        auto pending = owned.owner().group();
        for (usize index = 0; index < 2; ++index) {
            auto allocated = pending.allocate();
            if (!allocated) {
                return false;
            }
            pages[index] = allocated.value();
            pending.bytes(pages[index])[0] =
                static_cast<byte>(0x30 + index);
        }
        owned.append(std::move(pending));
    }
    const mm::Extent owned_extents[]{
        {
            .object = {0, 1},
            .physical = {pages[0], 1},
            .perms = read_only,
        },
        {
            .object = {1, 1},
            .physical = {pages[1], 1},
            .perms = read_only,
        },
    };

    auto image_ready = fixture.make(2 * mm::page_size, mm::PhysCfg{libk::Span<const mm::Extent>{owned_extents}, std::move(owned)});
    if (!image_ready) return false;
    auto& image = **image_ready;
    if (!image_ready) {
        return false;
    }
    {
        auto page = image.materialize(0);
        if (!page || page.value().page() != pages[0]
            || fixture.pmm().bytes(pages[0])[0] != byte{0x30}) {
            return false;
        }
        auto lease = std::move(page).value();
        image.retire();
        if (image.state() != mm::MemState::Stopping
            || fixture.pmm().state_of(pages[0]).value()
                != mm::PageState::Allocated) {
            return false;
        }
    }
    return image.state() == mm::MemState::Retired
        && fixture.pmm().state_of(pages[0]).value() == mm::PageState::Free
        && fixture.pmm().state_of(pages[1]).value() == mm::PageState::Free
        && fixture.pmm().free_page_count() == free_before;
}

bool test_reverse_attachment_drives_destroy_invalidation(
    const TestContext&) noexcept {
    MemoryFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }
    const usize free_before = fixture.pmm().free_page_count();

    auto memory_ready = fixture.make(2 * mm::page_size, mm::AnonCfg{});
    if (!memory_ready) return false;
    auto& memory = **memory_ready;
    mm::Page resident{};
    {
        auto page = memory.materialize(0);
        if (!page) {
            return false;
        }
        resident = page.value().page();
    }
    FakeMapping mapping{};
    if (!memory.attach(
            mapping.attachment,
            mm::Perms::of(
                mm::Perm::Read,
                mm::Perm::Write))) {
        return false;
    }
    memory.retire();
    if (mapping.invalidations != 1
        || !mapping.attachment.attached()
        || !mapping.attachment.busy()
        || memory.state() != mm::MemState::Stopping
        || memory.attachment_count() != 1
        || fixture.pmm().state_of(resident).value()
            != mm::PageState::Allocated) {
        return false;
    }
    if (mapping.attachment.detach()
        || memory.state() != mm::MemState::Retired
        || fixture.pmm().state_of(resident).value() != mm::PageState::Free
        || mapping.releases != 0) {
        return false;
    }
    mapping.work.reset();
    return mapping.releases == 1
        && !mapping.attachment.busy()
        && fixture.pmm().free_page_count() == free_before;
}

bool test_private_memory_initialization(const TestContext&) noexcept {
    MemoryFixture fixture{};
    if (!fixture.initialize()) return false;

    using namespace mm;
    auto memory_ready = fixture.make(2 * mm::page_size, AnonCfg{
        .perms = Perms::of(Perm::Read, Perm::Write, Perm::Execute)});
    if (!memory_ready) return false;
    auto& memory = **memory_ready;
    const byte data[]{byte{0x31}, byte{0x72}};
    if (!memory.write(17, {data, sizeof(data)})) return false;
    byte output[20]{};
    if (!memory.read(0, {output, sizeof(output)}) || output[16] != byte{}
        || output[17] != data[0] || output[18] != data[1] || output[19] != byte{}) return false;
    if (memory.write(page_size - 1, {data, sizeof(data)})) return false;
    {
        auto loan = memory.materialize(0);
        if (!loan || memory.write(0, {data, sizeof(data)}) || memory.begin_transfer(0)) return false;
    }
    FakeMapping reader{};
    if (!memory.attach(reader.attachment, Perms::of(Perm::Read))) return false;
    const auto while_mapped = memory.write(0, {data, sizeof(data)});
    if (while_mapped || while_mapped.error() != MemErr::Busy
        || !reader.attachment.detach()) return false;
    {
        auto transfer = memory.begin_transfer(0);
        if (!transfer || fixture.pmm().bytes(transfer.value().page())[17] != data[0]) return false;
        transfer.value().abort();
    }
    if (!memory.seal()) return false;
    const auto sealed = memory.write(0, {data, sizeof(data)});
    return !sealed && sealed.error() == MemErr::InvalidAccess;
}

bool test_executable_seal_closes_writable_attachments(
    const TestContext&) noexcept {
    MemoryFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }

    auto memory_ready = fixture.make(mm::page_size, mm::AnonCfg{
            .perms = mm::Perms::of(
                mm::Perm::Read,
                mm::Perm::Write,
                mm::Perm::Execute),
            .eager = true,
        });
    if (!memory_ready) return false;
    auto& memory = **memory_ready;
    FakeMapping writer{};
    if (!memory.attach(
            writer.attachment,
            mm::Perms::of(
                mm::Perm::Read, mm::Perm::Write))) {
        return false;
    }
    const auto busy = memory.seal();
    if (busy || busy.error() != mm::MemErr::Busy
        || memory.seal_state() != mm::SealState::Loadable
        || memory.content_epoch().raw != 0
        || !writer.attachment.detach()) {
        return false;
    }
    if (!memory.seal()
        || memory.seal_state() != mm::SealState::Executable
        || memory.content_epoch() != mm::ContentEpoch{1}) {
        return false;
    }
    FakeMapping executable{};
    FakeMapping late_writer{};
    const auto mapped = memory.attach(
        executable.attachment,
        mm::Perms::of(
            mm::Perm::Read, mm::Perm::Execute));
    const auto rejected = memory.attach(
        late_writer.attachment,
        mm::Perms::of(
            mm::Perm::Read, mm::Perm::Write));
    return mapped && !rejected
        && rejected.error() == mm::MemErr::InvalidAccess
        && executable.attachment.detach();
}

bool test_object_store_memory_lifecycle_waits_for_page_lease(
    const TestContext&) noexcept {
    MemoryFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }
    auto invalid = fixture.memory().get<mm::Mem>().create({}, fixture.pmm(), 1, mm::AnonCfg{});
    if (invalid || invalid.error() != mm::MemErr::InvalidSize) {
        return false;
    }
    auto pending = fixture.memory().get<mm::Mem>().create({}, fixture.pmm(), 2 * mm::page_size, mm::AnonCfg{});
    if (!pending) {
        return false;
    }
    fixture.keep(std::move(pending).value().publish());
    const auto id = fixture.pooled().id();
    auto pin_result = fixture.memory().get<mm::Mem>().lookup(id);
    if (!pin_result) {
        return false;
    }
    auto pin = std::move(pin_result).value();
    auto page_result = pin->materialize(0);
    if (!page_result) {
        return false;
    }
    auto page = std::move(page_result).value();
    if (!fixture.pooled().retire()) {
        return false;
    }
    fixture.release_pooled();
    if (pin->state() != mm::MemState::Stopping) {
        return false;
    }
    page.reset();
    if (pin->state() != mm::MemState::Retired) {
        return false;
    }
    pin.reset();
    while (memory_work->run()) {}
    return !fixture.memory().get<mm::Mem>().lookup(id)
        && fixture.pmm().verify_invariants();
}

bool test_pager_backing_donates_owned_page_without_copy(
    const TestContext&) noexcept {
    MemoryFixture fixture{};
    if (!fixture.initialize() || !fixture.make_pager()) {
        return false;
    }
    auto& pager = fixture.pager();

    const auto access = mm::Perms::of(
        mm::Perm::Read, mm::Perm::Write);
    auto pager_ref = fixture.pager_ref();
    if (!pager_ref) return false;
    auto memory_ready = fixture.make(2 * mm::page_size, mm::PagedCfg{std::move(pager_ref).value(), access});
    if (!memory_ready) return false;
    auto& memory = **memory_ready;
    const auto pending = memory.materialize(1);
    if (pending || pending.error() != mm::MemErr::Pending
        || memory.query(1).value() != mm::ContentState::Busy) {
        return false;
    }
    if (pager.pending() != 1) {
        return false;
    }
    const auto request = pager.claim();
    if (!request || request.value().page_index != 1) {
        return false;
    }
    auto allocated = fixture.pmm().allocate_page();
    if (!allocated) {
        return false;
    }
    const mm::Page donated = allocated.value().page();
    fixture.pmm().bytes(donated)[0] = byte{0x7a};
    if (!memory.supply(pager, request.value().id, std::move(allocated).value())) {
        return false;
    }
    auto resident = memory.materialize(1);
    if (!resident || resident.value().page() != donated
        || fixture.pmm().bytes(donated)[0] != byte{0x7a}) {
        return false;
    }
    resident.value().reset();
    memory.retire();
    const bool result = memory.state() == mm::MemState::Retired
        && fixture.pmm().state_of(donated).value()
            == mm::PageState::Free;
    return result;
}

bool test_pager_supply_moves_staging_owner(
    const TestContext&) noexcept {
    MemoryFixture fixture{};
    if (!fixture.initialize() || !fixture.make_pager()) {
        return false;
    }
    auto& pager = fixture.pager();
    StagingReset staging_reset{};


    const auto access = mm::Perms::of(
        mm::Perm::Read, mm::Perm::Write);
    auto target_pager = fixture.pager_ref();
    auto peer_pager = fixture.pager_ref();
    if (!target_pager) return false;
    auto target_ready = fixture.make(mm::page_size, mm::PagedCfg{std::move(target_pager).value(), access});
    if (!target_ready) return false;
    auto& target = **target_ready;
    if (!peer_pager) return false;
    auto peer_ready = fixture.make_peer(mm::page_size, mm::PagedCfg{std::move(peer_pager).value(), access});
    if (!peer_ready) return false;
    auto& peer = **peer_ready;
    auto staged = mm::Mem::prepare({}, fixture.pmm(), mm::page_size, mm::AnonCfg{.perms = access});
    if (!staged) return false;
    auto& staging = memory_test_staging.emplace(std::move(*staged));
    auto source_page = staging.materialize(0);
    if (!source_page) {
        return false;
    }
    const mm::Page donated = source_page.value().page();
    fixture.pmm().bytes(donated)[0] = byte{0x31};
    source_page.value().reset();
    auto pending = target.materialize(0);
    if (pending || pending.error() != mm::MemErr::Pending) {
        return false;
    }
    const auto peer_pending = peer.materialize(0);
    if (peer_pending
        || peer_pending.error() != mm::MemErr::Pending) {
        return false;
    }
    if (pager.pending() != 2) {
        return false;
    }
    auto request = pager.claim();
    if (!request) {
        return false;
    }
    auto transfer = staging.begin_transfer(0);
    if (!transfer) {
        return false;
    }
    if (peer.supply(pager, std::move(transfer).value(), request.value().id)
        || staging.query(0).value() != mm::ContentState::Resident) {
        return false;
    }
    auto retry_transfer = staging.begin_transfer(0);
    if (!retry_transfer) {
        return false;
    }
    if (!target.supply(pager, std::move(retry_transfer).value(), request.value().id)) {
        return false;
    }
    if (staging.query(0).value() != mm::ContentState::Zero) {
        return false;
    }
    auto resident = target.materialize(0);
    const bool result = resident
        && resident.value().page() == donated
        && fixture.pmm().bytes(donated)[0] == byte{0x31};
    if (resident) {
        resident.value().reset();
    }
    target.retire();
    const auto peer_request = pager.claim();
    if (peer_request) {
        if (!peer.pager_finish(pager, peer_request.value().id, true)) {
            return false;
        }
    }
    staging.retire();
    return result && fixture.pmm().state_of(donated).value()
        == mm::PageState::Free;
}

bool test_two_pager_backings_reject_colliding_claim_owner(
    const TestContext&) noexcept {
    MemoryFixture fixture{};
    if (!fixture.initialize() || !fixture.make_pager()) {
        return false;
    }
    auto& pager = fixture.pager();
    const auto access = mm::Perms::of(
        mm::Perm::Read, mm::Perm::Write);


    auto first_pager = fixture.pager_ref();
    auto second_pager = fixture.pager_ref();
    if (!first_pager) return false;
    auto first_ready = fixture.make(mm::page_size, mm::PagedCfg{std::move(first_pager).value(), access});
    if (!first_ready) return false;
    auto& first = **first_ready;
    if (!second_pager) return false;
    auto second_ready = fixture.make_peer(mm::page_size, mm::PagedCfg{std::move(second_pager).value(), access});
    if (!second_ready) return false;
    auto& second = **second_ready;
    if (!first_pager || !second_pager
        || !first_ready
        || !second_ready
        || first.materialize(0).error() != mm::MemErr::Pending
        || second.materialize(0).error() != mm::MemErr::Pending) {
        return false;
    }
    if (pager.pending() != 2) {
        return false;
    }
    const auto first_claim = pager.claim();
    const auto second_claim = pager.claim();
    if (!first_claim || !second_claim
        || first_claim.value().page_index != second_claim.value().page_index) {
        if (first_claim) {
            static_cast<void>(first.pager_finish(pager, first_claim.value().id, true));
        }
        if (second_claim) {
            static_cast<void>(second.pager_finish(pager, second_claim.value().id, true));
        }
        first.retire();
        second.retire();
        static_cast<void>(pager.close(true));
        return false;
    }
    const auto rejected_result = first.pager_finish(pager, second_claim.value().id, true);
    const bool first_busy = first.query(0).value()
        == mm::ContentState::Busy;
    const bool second_busy = second.query(0).value()
        == mm::ContentState::Busy;
    const auto first_failed = first.pager_finish(pager, first_claim.value().id, true);
    const auto second_failed = second.pager_finish(pager, second_claim.value().id, true);
    const bool protocol_ok = !rejected_result.has_value() && first_busy
        && second_busy && first_failed.has_value()
        && second_failed.has_value()
        && first.query(0).value() == mm::ContentState::Failed
        && second.query(0).value() == mm::ContentState::Failed;
    first.retire();
    second.retire();
    const bool retired = first.state() == mm::MemState::Retired
        && second.state() == mm::MemState::Retired;
    const bool closed = pager.close(false);
    if (!closed) {
        static_cast<void>(pager.close(true));
    }
    return protocol_ok && retired && closed;
}

bool test_pager_fail_publishes_backing_failure(
    const TestContext&) noexcept {
    MemoryFixture fixture{};
    if (!fixture.initialize() || !fixture.make_pager()) {
        return false;
    }
    auto& pager = fixture.pager();

    const auto access = mm::Perms::of(
        mm::Perm::Read, mm::Perm::Write);
    auto pager_ref = fixture.pager_ref();
    if (!pager_ref) return false;
    auto memory_ready = fixture.make(mm::page_size, mm::PagedCfg{std::move(pager_ref).value(), access});
    if (!memory_ready) return false;
    auto& memory = **memory_ready;
    const auto pending = memory.materialize(0);
    if (pending || pending.error() != mm::MemErr::Pending) {
        return false;
    }
    if (pager.pending() != 1) {
        return false;
    }
    const auto request = pager.claim();
    if (!request
        || !memory.pager_finish(pager, request.value().id, true)
        || memory.query(0).value() != mm::ContentState::Failed) {
        return false;
    }
    if (pager.reply(request.value().id)) {
        return false;
    }
    const auto failed = memory.materialize(0);
    if (failed || failed.error() != mm::MemErr::BackingFailed) {
        return false;
    }
    memory.retire();
    return memory.state() == mm::MemState::Retired;
}

bool test_pager_backing_rejects_wrong_pager(
    const TestContext&) noexcept {
    MemoryFixture fixture{};
    if (!fixture.initialize()
        || !fixture.make_pager()
        || !fixture.make_wrong_pager()) {
        return false;
    }
    auto& pager = fixture.pager();
    auto& wrong = fixture.wrong_pager();

    const auto access = mm::Perms::of(
        mm::Perm::Read, mm::Perm::Write);
    auto pager_ref = fixture.pager_ref();
    if (!pager_ref) return false;
    auto memory_ready = fixture.make(mm::page_size, mm::PagedCfg{std::move(pager_ref).value(), access});
    if (!memory_ready) return false;
    auto& memory = **memory_ready;
    if (memory.materialize(0).error() != mm::MemErr::Pending) {
        return false;
    }
    if (pager.pending() != 1) {
        return false;
    }
    const auto request = pager.claim();
    if (!request) {
        return false;
    }
    const auto rejected = memory.pager_finish(wrong, request.value().id, true);
    const auto finished = memory.pager_finish(pager, request.value().id, true);
    memory.retire();
    return !rejected
        && rejected.error() == mm::MemErr::OwnershipMismatch
        && finished && memory.state() == mm::MemState::Retired;
}

bool test_pager_backing_attach_failure_rolls_back(
    const TestContext&) noexcept {
    MemoryFixture fixture{};
    if (!fixture.initialize() || !fixture.make_pager()) {
        return false;
    }
    auto& pager = fixture.pager();
    if (!pager.close(false)) {
        return false;
    }

    const auto access = mm::Perms::of(
        mm::Perm::Read, mm::Perm::Write);
    auto pager_ref = fixture.pager_ref();
    if (!pager_ref) {
        return false;
    }
    const auto initialized = fixture.make(mm::page_size, mm::PagedCfg{std::move(pager_ref).value(), access});
    return !initialized
        && initialized.error() == mm::MemErr::AttachmentState;
}

bool test_pager_force_close_publishes_backing_failure(
    const TestContext&) noexcept {
    MemoryFixture fixture{};
    if (!fixture.initialize() || !fixture.make_pager()) {
        return false;
    }
    auto& pager = fixture.pager();

    const auto access = mm::Perms::of(
        mm::Perm::Read, mm::Perm::Write);
    auto pager_ref = fixture.pager_ref();
    if (!pager_ref) return false;
    auto memory_ready = fixture.make(mm::page_size, mm::PagedCfg{std::move(pager_ref).value(), access});
    if (!memory_ready) return false;
    auto& memory = **memory_ready;
    if (memory.materialize(0).error() != mm::MemErr::Pending) {
        return false;
    }
    if (pager.pending() != 1
        || !pager.claim()
        || !pager.close(true)
        || memory.query(0).value() != mm::ContentState::Failed) {
        return false;
    }
    memory.retire();
    return memory.state() == mm::MemState::Retired;
}

bool test_pager_lease_with_missing_sibling(const TestContext&) noexcept {
    using namespace mm;
    MemoryFixture fixture{};
    if (!fixture.initialize() || !fixture.make_pager()) return false;


    auto reference = fixture.pager_ref();
    if (!reference) return false;
    auto memory_ready = fixture.make(2 * page_size, mm::PagedCfg{std::move(reference).value(), Perms::of(Perm::Read)});
    if (!memory_ready) return false;
    auto& memory = **memory_ready;
    auto cleanup = libk::on_scope_exit([&memory]() noexcept { memory.retire(); });
    if (memory.materialize(0).error() != MemErr::Pending) return false;

    auto& pager = fixture.pager();
    const auto request = pager.claim();
    auto page = fixture.pmm().allocate_page();
    if (!request || !page || !memory.supply(pager, request.value().id, std::move(page).value())) return false;
    auto first = memory.materialize(0);
    auto second = memory.materialize(0);
    if (!first || !second || first.value().page() != second.value().page()
        || memory.materialize(1).error() != MemErr::Pending) return false;
    first.value().reset();
    if (memory.query(0).value() != ContentState::Resident) return false;
    memory.retire();
    if (memory.state() != MemState::Stopping ||
        first.value() || !second.value() || memory.materialize(0))
        return false;
    second.value().reset();
    return memory.state() == MemState::Retired;
}

bool test_pager_writeback_tracks_new_writes(
    const TestContext&) noexcept {
    MemoryFixture fixture{};
    if (!fixture.initialize() || !fixture.make_pager()) {
        return false;
    }
    auto& pager = fixture.pager();


    const auto access = mm::Perms::of(
        mm::Perm::Read, mm::Perm::Write);
    auto pager_ref = fixture.pager_ref();
    if (!pager_ref) return false;
    auto memory_ready = fixture.make(mm::page_size, mm::PagedCfg{std::move(pager_ref).value(), access});
    if (!memory_ready) return false;
    auto& memory = **memory_ready;
    auto cleanup = libk::on_scope_exit([&memory]() noexcept {
        memory.retire();
    });
    if (memory.materialize(0).error() != mm::MemErr::Pending) {
        return false;
    }
    if (pager.pending() != 1) {
        return false;
    }
    const auto request = pager.claim();
    auto page = fixture.pmm().allocate_page();
    if (!request || !page
        || !memory.supply(pager, request.value().id, std::move(page).value())) {
        return false;
    }

    if (!memory.observe_usage(0, true, true))
        return false;
    // Completion of an older snapshot must leave newer writes dirty.
    if (!memory.writeback(0)) {
        return false;
    }
    const auto writeback_claim = pager.claim();
    if (!writeback_claim
        || memory.pager_finish(pager, 0, false).error()
            != mm::MemErr::OwnershipMismatch
        || !memory.observe_usage(0, true, true)
        || !memory.pager_finish(pager, writeback_claim.value().id, false)) {
        return false;
    }
    if (!memory.writeback(0)) {
        return false;
    }
    const auto retry_claim = pager.claim();
    if (!retry_claim
        || !memory.pager_finish(pager, retry_claim.value().id, false)
        || memory.query(0).value() != mm::ContentState::Resident) {
        return false;
    }
    memory.retire();
    return memory.state() == mm::MemState::Retired;
}

bool test_private_pager_preserves_dirty_content(const TestContext&) noexcept {
    using namespace mm;
    MemoryFixture fixture{};
    if (!fixture.initialize() || !fixture.make_pager()) return false;


    auto reference = fixture.pager_ref();
    if (!reference) return false;
    auto memory_ready = fixture.make(2 * page_size, mm::PagedCfg{std::move(reference).value(), Perms::of(Perm::Read, Perm::Write), true});
    if (!memory_ready) return false;
    auto& memory = **memory_ready;
    auto cleanup = libk::on_scope_exit([&memory]() noexcept { memory.retire(); });
    auto& pager = fixture.pager();
    for (usize index = 0; index < 2; ++index) {
        if (memory.materialize(index).error() != MemErr::Pending) return false;

        const auto request = pager.claim();
        auto page = fixture.pmm().allocate_page();
        if (!request || !page) return false;
        page.value().bytes()[0] = byte{0x5a};
        if (!memory.supply(pager, request.value().id, std::move(page).value())) return false;
    }
    if (!memory.observe_usage(0, false, true)
        || memory.writeback(0).error() != MemErr::InvalidState) return false;
    byte contents[1]{};
    return memory.read(0, {contents, 1}) && contents[0] == byte{0x5a};
}

bool test_pager_forced_close_settles_backing_obligations(
    const TestContext&) noexcept {
    MemoryFixture fixture{};
    if (!fixture.initialize() || !fixture.make_pager()) {
        return false;
    }
    auto& pager = fixture.pager();


    const auto access = mm::Perms::of(
        mm::Perm::Read, mm::Perm::Write);
    auto pager_ref = fixture.pager_ref();
    if (!pager_ref) return false;
    auto memory_ready = fixture.make(2 * mm::page_size, mm::PagedCfg{std::move(pager_ref).value(), access});
    if (!memory_ready) return false;
    auto& memory = **memory_ready;
    auto cleanup = libk::on_scope_exit([&memory]() noexcept {
        memory.retire();
    });
    if (memory.materialize(0).error() != mm::MemErr::Pending) {
        return false;
    }
    if (pager.pending() != 1) {
        return false;
    }
    const auto request = pager.claim();
    auto page = fixture.pmm().allocate_page();
    if (!request || !page
        || !memory.supply(pager, request.value().id, std::move(page).value())
        || !memory.observe_usage(0, true, true)) {
        return false;
    }
    if (!memory.writeback(0)
        || pager.pending() != 1) {
        return false;
    }
    // A second page stays published-but-unclaimed so forced close also walks
    // the real page-in Forced branch, not only the writeback edge.
    if (memory.materialize(1).error() != mm::MemErr::Pending) {
        return false;
    }
    if (pager.pending() != 2) {
        return false;
    }
    const auto active = pager.claim();
    if (!active
        || active.value().kind != Pager::Kind::Writeback
        || memory.writeback(0).error() != mm::MemErr::InvalidState) {
        return false;
    }
    const auto queued = pager.claim();
    if (!queued
        || queued.value().kind != Pager::Kind::PageIn) {
        return false;
    }
    if (!pager.close(true) || pager.state() != Pager::State::Closed
        || memory.writeback(0).error() != mm::MemErr::InvalidState
        || memory.query(1).value() != mm::ContentState::Failed
        || pager.reply(active.value().id).error()
            != Pager::Error::Stale
        || pager.reply(queued.value().id).error()
            != Pager::Error::Stale
        || memory.pager_finish(pager, active.value().id, false)
               .has_value()
        || memory.pager_finish(pager, queued.value().id, true)
               .has_value()) {
        return false;
    }
    memory.retire();
    return memory.state() == mm::MemState::Retired
        && pager.state() == Pager::State::Closed;
}

} // namespace

static bool test_pager_requests_follow_mem_lifetime(const TestContext&) noexcept {
    using namespace mm;
    MemoryFixture f;
    if (!f.initialize() || !f.make_pager()) return false;

    auto& pager = f.pager();
    auto ref = f.pager_ref();
    if (!ref) return false;
    auto mem_ready = f.make(40 * page_size, mm::PagedCfg{std::move(*ref), Perms::of(Perm::Read)});
    if (!mem_ready) return false;
    auto& mem = **mem_ready;
    if (!ref || !mem_ready) return false;
    Pager::Claims worker;
    auto cleanup = libk::on_scope_exit([&]() noexcept { mem.retire(); (void)pager.close(true); });
    for (usize i=0; i<40; ++i)
        if (mem.materialize(i).error() != MemErr::Pending) return false;
    if (pager.pending() != 40) return false;
    auto first = pager.claim(&worker);
    auto second = pager.claim(&worker);
    if (!first || !second || worker.empty()) return false;
    auto busy = pager.reply(first->id);
    if (!busy) return false;
    worker.release();
    if (!worker.empty() || pager.reply(second->id)) return false;
    busy = Pager::Reply{}; // Abort hands the same storage back to the ready queue.
    if (pager.pending() != 40 || pager.reply(first->id)) return false;
    auto next = pager.claim();
    if (!next || next->id <= second->id) return false;
    auto held = pager.reply(next->id);
    if (!held || pager.close(true) || pager.state() != Pager::State::Forced) return false;
    mem.retire();
    if (mem.state() != MemState::Stopping) return false;
    held = Pager::Reply{}; // The final actual borrow permits backing retirement.
    return mem.state() == MemState::Retired && pager.state() == Pager::State::Closed;
}

static void page_wait_published(void *context, mm::WaitRc result) noexcept {
    *static_cast<mm::WaitRc *>(context) = result;
}

static bool
test_page_wait_relation_is_owner_storage_and_terminal_checked(const TestContext &) noexcept {
    mm::WaitQueue request{};
    mm::WaitRelation relation{};
    auto seen = mm::WaitRc::Canceled;
    if (!request.attach(relation, &seen, &page_wait_published)) {
        return false;
    }
    auto batch = request.take();
    auto ready = mm::WaitQueue::finish(batch, mm::WaitRc::Ready);
    if (!batch.empty() || relation.attached() || !ready.publish() || seen != mm::WaitRc::Ready)
        return false;
    const u64 previous_generation = relation.generation;
    if (!request.waiters.empty()) {
        return false;
    }
    if (!request.attach(relation, &seen, &page_wait_published)) {
        return false;
    }
    if (relation.generation == previous_generation) {
        return false;
    }
    mm::WaitClaim canceled{};
    if (request.detach(relation, previous_generation)) {
        return false;
    }
    batch = request.take();
    canceled = mm::WaitQueue::finish(batch, mm::WaitRc::Canceled);
    if (relation.attached() || !canceled.publish()) return false;
    return request.waiters.empty();
}

static bool test_page_request_drain_keeps_next_request(const TestContext &) noexcept {
    mm::WaitQueue request{};
    mm::WaitRelation relations[3]{};
    mm::WaitRc seen[3]{
        mm::WaitRc::Canceled,
        mm::WaitRc::Canceled,
        mm::WaitRc::Canceled,
    };
    for (usize index = 0; index < 3; ++index) {
        if (!request.attach(relations[index], &seen[index], &page_wait_published)) {
            return false;
        }
    }
    auto batch = request.take();
    mm::WaitRelation next{};
    auto next_seen = mm::WaitRc::Canceled;
    if (!request.attach(next, &next_seen, &page_wait_published)) return false;
    usize drained{};
    while (!batch.empty()) {
        if (request.detach(batch.front(), batch.front().generation)) return false;
        auto claim = mm::WaitQueue::finish(batch, mm::WaitRc::Ready);
        if (!claim.publish()) return false;
        ++drained;
    }
    return drained == 3 && seen[0] == mm::WaitRc::Ready && seen[1] == mm::WaitRc::Ready &&
           seen[2] == mm::WaitRc::Ready && next_seen == mm::WaitRc::Canceled &&
           request.detach(next, next.generation);

}

void register_memory_tests(TestRegistry& registry) noexcept {
    (void)registry.add("memory", "Content completion drains before relation reuse",
        test_page_wait_relation_is_owner_storage_and_terminal_checked);
    (void)registry.add("memory", "Content completion keeps next request and excludes cancellation",
        test_page_request_drain_keeps_next_request);

    (void)registry.add("memory", "Mem owns dynamic requests through worker exit and outstanding Reply retirement", test_pager_requests_follow_mem_lifetime);
    (void)registry.add("memory", "private page initialization excludes loans, mappings and sealed content",
        test_private_memory_initialization);
    (void)registry.add("memory", "I/O root maps only its range and refunds tables",
        test_io_root_isolated_range_and_refund);
    (void)registry.add(
        "memory",
        "anonymous sparse pages own zeroed resident frames",
        test_anonymous_sparse_pages_own_zeroed_frames);
    (void)registry.add(
        "memory",
        "physical backing borrows reserved RAM and device extents",
        test_physical_backing_borrows_reserved_and_device_extents);
    (void)registry.add(
        "memory",
        "boot image distinguishes borrowed and owned frame release",
        test_boot_image_distinguishes_borrowed_and_owned_frames);
    (void)registry.add(
        "memory",
        "reverse attachment drives destroy invalidation completion",
        test_reverse_attachment_drives_destroy_invalidation);
    (void)registry.add(
        "memory",
        "executable seal closes writable attachment admission",
        test_executable_seal_closes_writable_attachments);
    (void)registry.add(
        "memory",
        "MM object memory retirement waits for active page lease",
        test_object_store_memory_lifecycle_waits_for_page_lease);
    (void)registry.add(
        "memory",
        "pager backing donates an OwnedPage without copying",
        test_pager_backing_donates_owned_page_without_copy);
    (void)registry.add(
        "memory",
        "pager supply moves ownership from transfer-ready staging",
        test_pager_supply_moves_staging_owner);
    (void)registry.add(
        "memory",
        "two pager backings reject colliding claim owners",
        test_two_pager_backings_reject_colliding_claim_owner);
    (void)registry.add(
        "memory",
        "pager failure publishes the target backing failure",
        test_pager_fail_publishes_backing_failure);
    (void)registry.add(
        "memory",
        "pager backing rejects a reply through another Pager",
        test_pager_backing_rejects_wrong_pager);
    (void)registry.add(
        "memory",
        "pager backing rolls back failed attachment admission",
        test_pager_backing_attach_failure_rolls_back);
    (void)registry.add(
        "memory",
        "forced pager close publishes backing failure",
        test_pager_force_close_publishes_backing_failure);
    (void)registry.add(
        "memory",
        "pager writeback preserves newer writes",
        test_pager_writeback_tracks_new_writes);
    (void)registry.add("memory", "private pager preserves resident contents",
        test_private_pager_preserves_dirty_content);
    (void)registry.add(
        "memory",
        "resident borrow pins backing through close",
        test_pager_lease_with_missing_sibling);
    (void)registry.add(
        "memory",
        "forced pager close settles active writeback and pending page-in",
        test_pager_forced_close_settles_backing_obligations);
}
