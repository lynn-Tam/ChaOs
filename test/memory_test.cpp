#include <test/test.hpp>

#include <libk/manual_lifetime.hpp>
#include <libk/noncopyable.hpp>
#include <libk/span.hpp>
#include <libk/utility.hpp>
#include <mm/memory_object.hpp>
#include <pager/pager.hpp>
#include <object/object_store.hpp>
#include <mm/vspace_work.hpp>
#include <core/kernel_image.hpp>

namespace {

constexpr usize memory_test_pages = 160;
constexpr usize reserved_pages = 8;
alignas(kernel::mm::page_size) byte
    memory_test_ram[memory_test_pages * kernel::mm::page_size]{};
constinit libk::ManualLifetime<kernel::mm::RegionList> memory_test_map{};
constinit libk::ManualLifetime<kernel::mm::DirectMap> memory_test_direct{};
constinit libk::ManualLifetime<kernel::mm::Pmm> memory_test_pmm{};
constinit libk::ManualLifetime<kernel::object::ObjectStore>
    memory_test_objects{};
constinit libk::ManualLifetime<kernel::mm::VSpaceExecutor>
    memory_test_vspace_work{};
constinit libk::ManualLifetime<kernel::mm::MemoryObject> memory_test_object{};
constinit libk::ManualLifetime<kernel::mm::MemoryObject> memory_test_staging{};
constinit libk::ManualLifetime<kernel::pager::Pager> memory_test_pager{};

struct PagerReset final {
    ~PagerReset() noexcept { memory_test_pager.reset(); }
};

struct StagingReset final {
    ~StagingReset() noexcept { memory_test_staging.reset(); }
};

[[nodiscard]] auto page_at(usize offset) noexcept -> kernel::mm::Page {
    const auto physical = kernel::image::linked_physical(kernel::mm::VirtAddr{
        reinterpret_cast<usize>(memory_test_ram)});
    KASSERT(physical);
    const auto address = physical->checked_add(offset * kernel::mm::page_size);
    KASSERT(address);
    const auto page = kernel::mm::Page::from_base(*address);
    KASSERT(page);
    return *page;
}

class MemoryFixture final : private libk::noncopyable_nonmovable {
public:
    MemoryFixture() noexcept = default;
    ~MemoryFixture() noexcept { reset(); }

    [[nodiscard]] auto initialize() noexcept -> bool {
        reset();
        const auto physical = kernel::image::linked_physical(kernel::mm::VirtAddr{
            reinterpret_cast<usize>(memory_test_ram)});
        if (!physical) {
            return false;
        }
        auto& map = memory_test_map.emplace();
        if (!map.try_emplace_back(kernel::mm::Region{
                kernel::mm::PageRange{page_at(0), reserved_pages},
                kernel::mm::RegionKind::KernelImage})
            || !map.try_emplace_back(kernel::mm::Region{
                kernel::mm::PageRange{
                    page_at(reserved_pages),
                    memory_test_pages - reserved_pages},
                kernel::mm::RegionKind::AvailableRam})) {
            reset();
            return false;
        }
        const auto direct = kernel::mm::DirectMap::initialize_in(
            memory_test_direct,
            map,
            kernel::mm::DirectMapLayout{
                .physical_base = *physical,
                .virtual_base = kernel::mm::VirtAddr{
                    reinterpret_cast<usize>(memory_test_ram)},
                .window_size = sizeof(memory_test_ram),
            });
        if (!direct
            || !kernel::mm::Pmm::initialize_in(
                memory_test_pmm,
                *memory_test_direct,
                libk::move(map))) {
            reset();
            return false;
        }
        memory_test_map.reset();
        auto& vspace_work = memory_test_vspace_work.emplace();
        [[maybe_unused]] auto& objects =
            memory_test_objects.emplace(*memory_test_pmm, vspace_work);
        return true;
    }

    [[nodiscard]] auto make(usize byte_size) noexcept -> kernel::mm::MemoryObject& {
        if (memory_test_object) {
            memory_test_object->retire();
            memory_test_object.reset();
        }
        return memory_test_object.emplace(*memory_test_pmm, byte_size);
    }

    [[nodiscard]] auto pmm() noexcept -> kernel::mm::Pmm& {
        return *memory_test_pmm;
    }

    [[nodiscard]] auto objects() noexcept
        -> kernel::object::ObjectStore& {
        return *memory_test_objects;
    }

    void keep(kernel::object::ObjectStore::MemoryHold&& memory) noexcept {
        KASSERT(!pooled_);
        pooled_ = libk::move(memory);
    }

    [[nodiscard]] auto pooled() noexcept
        -> kernel::object::ObjectStore::MemoryHold& {
        return pooled_;
    }

    void release_pooled() noexcept { pooled_.reset(); }

private:
    void reset() noexcept {
        if (memory_test_object) {
            memory_test_object->retire();
            memory_test_object.reset();
        }
        if (pooled_) {
            (void)pooled_.retire();
            pooled_.reset();
        }
        if (memory_test_objects) {
            memory_test_objects->drain_reclaim();
        }
        memory_test_objects.reset();
        memory_test_vspace_work.reset();
        memory_test_pmm.reset();
        memory_test_direct.reset();
        memory_test_map.reset();
    }

    kernel::object::ObjectStore::MemoryHold pooled_{};
};

struct FakeMapping final : private libk::noncopyable_nonmovable {
    FakeMapping() noexcept : attachment(this, ops) {}

    static void invalidate(
        void* context,
        kernel::mm::MemoryWork&& work,
        kernel::mm::MemoryInvalidation reason) noexcept {
        auto& self = *static_cast<FakeMapping*>(context);
        KASSERT(reason == kernel::mm::MemoryInvalidation::Destroy);
        ++self.invalidations;
        self.work = libk::move(work);
    }

    static void released(void* context) noexcept {
        ++static_cast<FakeMapping*>(context)->releases;
    }

    inline static const kernel::mm::MemoryAttachmentOps ops{
        invalidate,
        released,
    };

    kernel::mm::MemoryAttachment attachment;
    kernel::mm::MemoryWork work{};
    usize invalidations{};
    usize releases{};
};

bool test_anonymous_sparse_pages_own_zeroed_frames(
    const TestContext&) noexcept {
    MemoryFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }
    const usize free_before = fixture.pmm().free_page_count();
    kernel::mm::MemoryObject& memory = fixture.make(8 * kernel::mm::page_size);
    if (!memory.initialize_anonymous({})
        || memory.kind() != kernel::mm::BackingKind::Anonymous
        || memory.query(5).value() != kernel::mm::ContentState::Zero) {
        return false;
    }

    kernel::mm::Page resident{};
    {
        auto materialized = memory.materialize(5);
        if (!materialized) {
            return false;
        }
        auto lease = libk::move(materialized).value();
        resident = lease.page().page;
        const byte* const bytes = fixture.pmm().bytes(resident);
        if (bytes[0] != 0 || bytes[kernel::mm::page_size - 1] != 0
            || !lease.page().access.contains(kernel::mm::Access::Write)
            || lease.page().type != kernel::mm::MemoryType::Normal) {
            return false;
        }
        fixture.pmm().bytes(resident)[37] = byte{0x5a};
    }
    {
        auto materialized = memory.materialize(5);
        if (!materialized
            || materialized.value().page().page != resident
            || fixture.pmm().bytes(resident)[37] != byte{0x5a}
            || memory.query(3).value() != kernel::mm::ContentState::Zero) {
            return false;
        }
        auto lease = libk::move(materialized).value();
        memory.retire();
        if (memory.state() != kernel::mm::MemoryState::Stopping
            || fixture.pmm().state_of(resident).value()
                != kernel::mm::PageState::Allocated) {
            return false;
        }
    }
    const bool lazy_complete = memory.state() == kernel::mm::MemoryState::Retired
        && fixture.pmm().state_of(resident).value() == kernel::mm::PageState::Free
        && fixture.pmm().free_page_count() == free_before
        && fixture.pmm().verify_invariants();
    if (!lazy_complete) {
        return false;
    }

    kernel::mm::MemoryObject& eager = fixture.make(3 * kernel::mm::page_size);
    if (!eager.initialize_anonymous(kernel::mm::AnonymousConfig{
            .access = kernel::mm::AccessMask::of(kernel::mm::Access::Read, kernel::mm::Access::Write),
            .eager = true,
        })) {
        return false;
    }
    for (usize index = 0; index < eager.page_count(); ++index) {
        auto state = eager.query(index);
        if (!state || state.value() != kernel::mm::ContentState::Resident) {
            return false;
        }
    }
    eager.retire();
    return eager.state() == kernel::mm::MemoryState::Retired
        && fixture.pmm().free_page_count() == free_before;
}

bool test_physical_backing_borrows_reserved_and_device_extents(
    const TestContext&) noexcept {
    MemoryFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }
    const usize free_before = fixture.pmm().free_page_count();
    constexpr auto read_execute = kernel::mm::AccessMask::of(
        kernel::mm::Access::Read, kernel::mm::Access::Execute);
    constexpr auto read_write = kernel::mm::AccessMask::of(
        kernel::mm::Access::Read, kernel::mm::Access::Write);
    const kernel::mm::Page device = *kernel::mm::Page::from_base(kernel::mm::PhysAddr{0x1000'0000});
    const kernel::mm::MemoryExtent extents[]{
        {
            .object = {0, 2},
            .physical = {page_at(0), 2},
            .access = read_execute,
            .type = kernel::mm::MemoryType::Normal,
        },
        {
            .object = {2, 1},
            .physical = {device, 1},
            .access = read_write,
            .type = kernel::mm::MemoryType::Device,
        },
    };
    kernel::mm::MemoryObject& memory = fixture.make(3 * kernel::mm::page_size);
    if (!memory.initialize_physical(libk::Span<const kernel::mm::MemoryExtent>{extents})) {
        return false;
    }
    {
        auto code = memory.materialize(1);
        auto mmio = memory.materialize(2);
        if (!code || !mmio
            || code.value().page().page != page_at(1)
            || code.value().page().access != read_execute
            || mmio.value().page().page != device
            || mmio.value().page().type != kernel::mm::MemoryType::Device) {
            return false;
        }
    }
    memory.retire();
    if (fixture.pmm().state_of(page_at(0)).value()
            != kernel::mm::PageState::Reserved
        || fixture.pmm().free_page_count() != free_before) {
        return false;
    }

    kernel::mm::MemoryObject& invalid = fixture.make(kernel::mm::page_size);
    const kernel::mm::MemoryExtent free_extent[]{
        {
            .object = {0, 1},
            .physical = {page_at(reserved_pages + 20), 1},
            .access = read_write,
            .type = kernel::mm::MemoryType::Normal,
        },
    };
    const auto rejected = invalid.initialize_physical(
        libk::Span<const kernel::mm::MemoryExtent>{free_extent});
    if (rejected
        || rejected.error() != kernel::mm::MemoryError::OwnershipMismatch
        || invalid.state() != kernel::mm::MemoryState::Retired) {
        return false;
    }

    kernel::mm::MemoryObject& conflicting = fixture.make(2 * kernel::mm::page_size);
    const kernel::mm::MemoryExtent conflicting_extents[]{
        {
            .object = {0, 1},
            .physical = {device, 1},
            .access = read_write,
            .type = kernel::mm::MemoryType::Device,
        },
        {
            .object = {1, 1},
            .physical = {device, 1},
            .access = read_write,
            .type = kernel::mm::MemoryType::Uncached,
        },
    };
    const auto alias = conflicting.initialize_physical(
        libk::Span<const kernel::mm::MemoryExtent>{conflicting_extents});
    return !alias
        && alias.error() == kernel::mm::MemoryError::InvalidMemoryType
        && conflicting.state() == kernel::mm::MemoryState::Retired;
}

bool test_boot_image_distinguishes_borrowed_and_owned_frames(
    const TestContext&) noexcept {
    MemoryFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }
    constexpr auto read_only = kernel::mm::AccessMask::of(kernel::mm::Access::Read);
    const kernel::mm::MemoryExtent borrowed_extent[]{
        {
            .object = {0, 1},
            .physical = {page_at(2), 1},
            .access = read_only,
            .type = kernel::mm::MemoryType::Normal,
        },
    };
    kernel::mm::MemoryObject& borrowed = fixture.make(kernel::mm::page_size);
    if (!borrowed.initialize_boot_image(
            libk::Span<const kernel::mm::MemoryExtent>{borrowed_extent},
            kernel::mm::BootOwnership::Borrowed)) {
        return false;
    }
    borrowed.retire();
    if (fixture.pmm().state_of(page_at(2)).value()
        != kernel::mm::PageState::Reserved) {
        return false;
    }
    memory_test_object.reset();

    const usize free_before = fixture.pmm().free_page_count();
    auto owned = fixture.pmm().make_page_group();
    kernel::mm::Page pages[2]{};
    {
        auto extension = owned.extend();
        for (usize index = 0; index < 2; ++index) {
            auto allocated = extension.allocate_page();
            if (!allocated) {
                return false;
            }
            pages[index] = allocated.value();
            extension.bytes(pages[index])[0] =
                static_cast<byte>(0x30 + index);
        }
        extension.commit();
    }
    const kernel::mm::MemoryExtent owned_extents[]{
        {
            .object = {0, 1},
            .physical = {pages[0], 1},
            .access = read_only,
            .type = kernel::mm::MemoryType::Normal,
        },
        {
            .object = {1, 1},
            .physical = {pages[1], 1},
            .access = read_only,
            .type = kernel::mm::MemoryType::Normal,
        },
    };
    kernel::mm::MemoryObject& image = fixture.make(2 * kernel::mm::page_size);
    if (!image.initialize_boot_image(
            libk::Span<const kernel::mm::MemoryExtent>{owned_extents},
            kernel::mm::BootOwnership::Owned,
            libk::move(owned))) {
        return false;
    }
    {
        auto page = image.materialize(0);
        if (!page || page.value().page().page != pages[0]
            || fixture.pmm().bytes(pages[0])[0] != byte{0x30}) {
            return false;
        }
        auto lease = libk::move(page).value();
        image.retire();
        if (image.state() != kernel::mm::MemoryState::Stopping
            || fixture.pmm().state_of(pages[0]).value()
                != kernel::mm::PageState::Allocated) {
            return false;
        }
    }
    return image.state() == kernel::mm::MemoryState::Retired
        && fixture.pmm().state_of(pages[0]).value() == kernel::mm::PageState::Free
        && fixture.pmm().state_of(pages[1]).value() == kernel::mm::PageState::Free
        && fixture.pmm().free_page_count() == free_before;
}

bool test_reverse_attachment_drives_destroy_invalidation(
    const TestContext&) noexcept {
    MemoryFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }
    const usize free_before = fixture.pmm().free_page_count();
    kernel::mm::MemoryObject& memory = fixture.make(2 * kernel::mm::page_size);
    if (!memory.initialize_anonymous({})) {
        return false;
    }
    kernel::mm::Page resident{};
    {
        auto page = memory.materialize(0);
        if (!page) {
            return false;
        }
        resident = page.value().page().page;
    }
    FakeMapping mapping{};
    if (!memory.attach(
            mapping.attachment,
            kernel::mm::AccessMask::of(
                kernel::mm::Access::Read,
                kernel::mm::Access::Write))) {
        return false;
    }
    memory.retire();
    if (mapping.invalidations != 1
        || !mapping.attachment.attached()
        || !mapping.attachment.busy()
        || memory.state() != kernel::mm::MemoryState::Stopping
        || memory.attachment_count() != 1
        || fixture.pmm().state_of(resident).value()
            != kernel::mm::PageState::Allocated) {
        return false;
    }
    if (mapping.attachment.detach()
        || memory.state() != kernel::mm::MemoryState::Retired
        || fixture.pmm().state_of(resident).value() != kernel::mm::PageState::Free
        || mapping.releases != 0) {
        return false;
    }
    mapping.work.reset();
    return mapping.releases == 1
        && !mapping.attachment.busy()
        && fixture.pmm().free_page_count() == free_before;
}

bool test_executable_seal_closes_writable_attachments(
    const TestContext&) noexcept {
    MemoryFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }
    kernel::mm::MemoryObject& memory = fixture.make(kernel::mm::page_size);
    if (!memory.initialize_anonymous(kernel::mm::AnonymousConfig{
            .access = kernel::mm::AccessMask::of(
                kernel::mm::Access::Read,
                kernel::mm::Access::Write,
                kernel::mm::Access::Execute),
            .eager = true,
        })) {
        return false;
    }
    FakeMapping writer{};
    if (!memory.attach(
            writer.attachment,
            kernel::mm::AccessMask::of(
                kernel::mm::Access::Read, kernel::mm::Access::Write))) {
        return false;
    }
    const auto busy = memory.seal();
    if (busy || busy.error() != kernel::mm::MemoryError::Busy
        || memory.seal_state() != kernel::mm::SealState::Loadable
        || memory.content_epoch().raw != 0
        || !writer.attachment.detach()) {
        return false;
    }
    if (!memory.seal()
        || memory.seal_state() != kernel::mm::SealState::Executable
        || memory.content_epoch() != kernel::mm::ContentEpoch{1}) {
        return false;
    }
    FakeMapping executable{};
    FakeMapping late_writer{};
    const auto mapped = memory.attach(
        executable.attachment,
        kernel::mm::AccessMask::of(
            kernel::mm::Access::Read, kernel::mm::Access::Execute));
    const auto rejected = memory.attach(
        late_writer.attachment,
        kernel::mm::AccessMask::of(
            kernel::mm::Access::Read, kernel::mm::Access::Write));
    return mapped && !rejected
        && rejected.error() == kernel::mm::MemoryError::InvalidAccess
        && executable.attachment.detach();
}

bool test_object_store_memory_lifecycle_waits_for_page_lease(
    const TestContext&) noexcept {
    MemoryFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }
    auto invalid = fixture.objects().create_anonymous(1);
    if (invalid || invalid.error() != kernel::mm::MemoryError::InvalidSize) {
        return false;
    }
    auto pending = fixture.objects().create_anonymous(2 * kernel::mm::page_size);
    if (!pending) {
        return false;
    }
    fixture.keep(libk::move(pending).value().publish());
    const auto id = fixture.pooled().id();
    auto pin_result = fixture.objects().pin_memory(id);
    if (!pin_result) {
        return false;
    }
    auto pin = libk::move(pin_result).value();
    auto page_result = pin->materialize(0);
    if (!page_result) {
        return false;
    }
    auto page = libk::move(page_result).value();
    if (!fixture.pooled().retire()) {
        return false;
    }
    fixture.release_pooled();
    if (pin->state() != kernel::mm::MemoryState::Stopping) {
        return false;
    }
    page.reset();
    if (pin->state() != kernel::mm::MemoryState::Retired) {
        return false;
    }
    pin.reset();
    fixture.objects().drain_reclaim();
    return !fixture.objects().hold_memory(id)
        && fixture.pmm().verify_invariants();
}

bool test_pager_backing_donates_owned_page_without_copy(
    const TestContext&) noexcept {
    MemoryFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }
    auto& pager = memory_test_pager.emplace();
    PagerReset pager_reset{};
    kernel::mm::MemoryObject& memory = fixture.make(2 * kernel::mm::page_size);
    const auto access = kernel::mm::AccessMask::of(
        kernel::mm::Access::Read, kernel::mm::Access::Write);
    if (!memory.initialize_pager(pager, access)) {
        return false;
    }
    const auto pending = memory.materialize(1);
    if (pending || pending.error() != kernel::mm::MemoryError::Pending
        || memory.query(1).value() != kernel::mm::ContentState::Busy) {
        return false;
    }
    const auto request = pager.try_claim();
    if (!request || request.value().page_key.index != 1) {
        return false;
    }
    auto allocated = fixture.pmm().allocate_page();
    if (!allocated) {
        return false;
    }
    const kernel::mm::Page donated = allocated.value().page();
    fixture.pmm().bytes(donated)[0] = byte{0x7a};
    if (!memory.pager_supply(
            1,
            request.value().page_key.generation,
            request.value().key.generation,
            libk::move(allocated).value(),
            1)
        || !pager.complete(request.value().key)) {
        return false;
    }
    auto resident = memory.materialize(1);
    if (!resident || resident.value().page().page != donated
        || fixture.pmm().bytes(donated)[0] != byte{0x7a}) {
        return false;
    }
    resident.value().reset();
    memory.retire();
    const bool result = memory.state() == kernel::mm::MemoryState::Retired
        && fixture.pmm().state_of(donated).value()
            == kernel::mm::PageState::Free;
    return result;
}

bool test_pager_supply_moves_staging_owner(
    const TestContext&) noexcept {
    MemoryFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }
    auto& pager = memory_test_pager.emplace();
    PagerReset pager_reset{};
    StagingReset staging_reset{};
    kernel::mm::MemoryObject& target =
        fixture.make(kernel::mm::page_size);
    const auto access = kernel::mm::AccessMask::of(
        kernel::mm::Access::Read, kernel::mm::Access::Write);
    if (!target.initialize_pager(pager, access)) {
        return false;
    }
    auto& staging = memory_test_staging.emplace(
        fixture.pmm(), kernel::mm::page_size);
    if (!staging.initialize_anonymous(
            kernel::mm::AnonymousConfig{.access = access})) {
        return false;
    }
    auto source_page = staging.materialize(0);
    if (!source_page) {
        return false;
    }
    const kernel::mm::Page donated = source_page.value().page().page;
    fixture.pmm().bytes(donated)[0] = byte{0x31};
    source_page.value().reset();
    auto pending = target.materialize(0);
    if (pending || pending.error() != kernel::mm::MemoryError::Pending) {
        return false;
    }
    auto request = pager.try_claim();
    if (!request) {
        return false;
    }
    auto transfer = staging.begin_transfer(0);
    if (!transfer) {
        return false;
    }
    if (!target.pager_supply_transfer(
            libk::move(transfer).value(), 0,
            request.value().page_key.generation,
            request.value().key.generation,
            request.value().page_key.generation)
        || !pager.complete(request.value().key)) {
        return false;
    }
    if (staging.query(0).value() != kernel::mm::ContentState::Zero) {
        return false;
    }
    auto resident = target.materialize(0);
    const bool result = resident
        && resident.value().page().page == donated
        && fixture.pmm().bytes(donated)[0] == byte{0x31};
    if (resident) {
        resident.value().reset();
    }
    target.retire();
    staging.retire();
    return result && fixture.pmm().state_of(donated).value()
        == kernel::mm::PageState::Free;
}

bool test_pager_fail_publishes_backing_failure(
    const TestContext&) noexcept {
    MemoryFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }
    auto& pager = memory_test_pager.emplace();
    PagerReset pager_reset{};
    kernel::mm::MemoryObject& memory = fixture.make(kernel::mm::page_size);
    const auto access = kernel::mm::AccessMask::of(
        kernel::mm::Access::Read, kernel::mm::Access::Write);
    if (!memory.initialize_pager(pager, access)) {
        return false;
    }
    const auto pending = memory.materialize(0);
    if (pending || pending.error() != kernel::mm::MemoryError::Pending) {
        return false;
    }
    const auto request = pager.try_claim();
    if (!request
        || !memory.pager_fail(
            0,
            request.value().page_key.generation,
            request.value().key.generation)
        || memory.query(0).value() != kernel::mm::ContentState::Failed) {
        return false;
    }
    if (pager.complete(request.value().key)) {
        return false;
    }
    const auto failed = memory.materialize(0);
    if (failed || failed.error() != kernel::mm::MemoryError::BackingFailed) {
        return false;
    }
    memory.retire();
    return memory.state() == kernel::mm::MemoryState::Retired;
}

bool test_pager_reverse_mapping_usage_and_eviction(
    const TestContext&) noexcept {
    MemoryFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }
    auto& pager = memory_test_pager.emplace();
    PagerReset pager_reset{};
    kernel::mm::MemoryObject& memory = fixture.make(kernel::mm::page_size);
    const auto access = kernel::mm::AccessMask::of(
        kernel::mm::Access::Read, kernel::mm::Access::Write);
    if (!memory.initialize_pager(pager, access)
        || memory.materialize(0).error() != kernel::mm::MemoryError::Pending) {
        return false;
    }
    const auto request = pager.try_claim();
    auto page = fixture.pmm().allocate_page();
    if (!request || !page
        || !memory.pager_supply(
            0,
            request.value().page_key.generation,
            request.value().key.generation,
            libk::move(page).value(),
            1)
        || !pager.complete(request.value().key)) {
        return false;
    }

    auto lease = memory.materialize(0);
    if (!lease
        || memory.evict_page(0).error() != kernel::mm::MemoryError::Busy
        || !memory.observe_usage(0, true, true)
        || memory.page_state(0).value()
            != kernel::mm::PageSlotState::ResidentDirty) {
        return false;
    }
    lease.value().reset();

    kernel::mm::PageMapping mapping{};
    if (!memory.bind_mapping(mapping, 0)
        || !mapping.attached()
        || memory.evict_page(0).error() != kernel::mm::MemoryError::InvalidState) {
        return false;
    }
    // A dirty resident page cannot be evicted merely because its mapping is
    // removed; writeback owns the clean transition.
    if (!memory.unbind_mapping(mapping)
        || mapping.attached()
        || memory.evict_page(0).error() != kernel::mm::MemoryError::InvalidState) {
        return false;
    }
    const auto epoch = memory.queue_writeback(0);
    if (!epoch || !memory.begin_writeback(0, epoch.value())
        || !memory.observe_usage(0, true, true)
        || !memory.complete_writeback(0, epoch.value(), true)
        || memory.page_state(0).value()
            != kernel::mm::PageSlotState::ResidentDirty) {
        return false;
    }
    const auto retry = memory.queue_writeback(0);
    if (!retry || !memory.begin_writeback(0, retry.value())
        || !memory.complete_writeback(0, retry.value(), true)
        || memory.page_state(0).value()
            != kernel::mm::PageSlotState::ResidentClean
        || !memory.evict_page(0)
        || memory.page_state(0).value()
            != kernel::mm::PageSlotState::Missing) {
        return false;
    }
    memory.retire();
    return memory.state() == kernel::mm::MemoryState::Retired;
}

} // namespace

void register_memory_tests(TestRegistry& registry) noexcept {
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
        "ObjectStore memory retirement waits for active page lease",
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
        "pager failure publishes the target backing failure",
        test_pager_fail_publishes_backing_failure);
    (void)registry.add(
        "memory",
        "pager backing tracks mappings, usage, writeback, and eviction",
        test_pager_reverse_mapping_usage_and_eviction);
}
