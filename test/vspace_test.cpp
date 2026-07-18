#include <test/test.hpp>

#include <mm/virtual_layout.hpp>
#include <cap/cspace.hpp>
#include <cap/grant_graph.hpp>
#include <libk/manual_lifetime.hpp>
#include <libk/noncopyable.hpp>
#include <libk/utility.hpp>
#include <mm/kernel_vspace.hpp>
#include <mm/vspace.hpp>
#include <mm/vspace_work.hpp>
#include <object/object_store.hpp>
#include <resource/pool.hpp>
#include <resource/traits.hpp>
#include <core/kernel_image.hpp>
#include <execution/binding.hpp>

//Confirmatory experiment.
// Exit condition: retained as the permanent semantic/PTE/revocation contract
// once D5-D6 are integrated into the runtime dispatcher and syscall tests.

namespace {

constexpr usize vspace_test_page_count = 384;
alignas(kernel::mm::page_size) byte
    vspace_test_ram[vspace_test_page_count * kernel::mm::page_size]{};
constinit libk::ManualLifetime<kernel::mm::RegionList> vspace_test_map{};
constinit libk::ManualLifetime<kernel::mm::DirectMap> vspace_test_direct{};
constinit libk::ManualLifetime<kernel::mm::Pmm> vspace_test_pmm{};
constinit libk::ManualLifetime<kernel::mm::KernelVSpace> vspace_test_kernel{};
constinit libk::ManualLifetime<kernel::object::ObjectStore>
    vspace_test_objects{};
constinit libk::ManualLifetime<kernel::mm::VSpaceExecutor> vspace_test_work{};

class VSpaceFixture final : private libk::noncopyable_nonmovable {
public:
    VSpaceFixture() noexcept = default;
    ~VSpaceFixture() noexcept { reset(); }

    [[nodiscard]] auto initialize(bool eager, usize pages = 4) noexcept
        -> bool {
        reset();
        const auto physical = kernel::image::linked_physical(kernel::mm::VirtAddr{
            reinterpret_cast<usize>(vspace_test_ram)});
        if (!physical) {
            return false;
        }
        const auto first = kernel::mm::Page::from_base(*physical);
        if (!first) {
            return false;
        }
        auto& map = vspace_test_map.emplace();
        if (!map.try_emplace_back(kernel::mm::Region{
                kernel::mm::PageRange{*first, vspace_test_page_count},
                kernel::mm::RegionKind::AvailableRam})) {
            return false;
        }
        const auto direct = kernel::mm::DirectMap::initialize_in(
            vspace_test_direct,
            map,
            kernel::mm::DirectMapLayout{
                .physical_base = kernel::mm::PhysAddr{0},
                .virtual_base = kernel::mm::VirtAddr{kernel::mm::layout::DirectMapBegin},
                .window_size = kernel::mm::layout::DirectMapSize,
            });
        if (!direct
            || !kernel::mm::Pmm::initialize_in(
                vspace_test_pmm,
                *vspace_test_direct,
                libk::move(map))) {
            reset();
            return false;
        }
        vspace_test_map.reset();
        if (!kernel::mm::KernelVSpace::build_in(
                vspace_test_kernel, *vspace_test_pmm)) {
            reset();
            return false;
        }
        auto& work = vspace_test_work.emplace();
        auto& objects = vspace_test_objects.emplace(
            *vspace_test_pmm, work);
        auto memory = objects.create_anonymous(
            pages * kernel::mm::page_size,
            kernel::mm::AnonymousConfig{
                .access = kernel::mm::AccessMask::of(
                    kernel::mm::Access::Read, kernel::mm::Access::Write),
                .eager = eager,
            });
        if (!memory) {
            reset();
            return false;
        }
        memory_ = libk::move(memory).value().publish();
        auto space = objects.create_vspace(*vspace_test_kernel);
        if (!space) {
            reset();
            return false;
        }
        space_ = libk::move(space).value().publish();
        auto cspace = objects.create_cspace();
        if (!cspace) {
            reset();
            return false;
        }
        cspace_ = libk::move(cspace).value().publish();
        return true;
    }

    [[nodiscard]] auto map_kernel(
        kernel::mm::VirtRange range,
        kernel::mm::ObjectRange object,
        kernel::mm::AccessMask access) noexcept
        -> libk::Expected<kernel::mm::MapResult, kernel::mm::VSpaceError> {
        auto reference = memory_.ref();
        if (!reference) {
            return libk::unexpected(kernel::mm::VSpaceError::InvalidState);
        }
        return space_->map_kernel(
            context(),
            space_->root_key(),
            kernel::mm::MapRequest{range, object, access},
            libk::move(reference).value(),
            memory_.get(),
            memory_authority(memory_.get().page_count()));
    }

    [[nodiscard]] static auto memory_authority(usize pages) noexcept
        -> kernel::cap::MemoryAuthority {
        return kernel::cap::MemoryAuthority{
            .range = kernel::mm::ObjectRange{0, pages},
            .access = kernel::mm::AccessMask::of(
                kernel::mm::Access::Read, kernel::mm::Access::Write),
            .types = kernel::mm::MemoryTypes::of(kernel::mm::MemoryType::Normal),
        };
    }

    [[nodiscard]] auto root_authority() noexcept
        -> kernel::cap::VSpaceAuthority {
        return kernel::cap::VSpaceAuthority{
            .region = space_->root_key(),
            .range = kernel::mm::VirtRange{
                kernel::mm::VirtAddr{kernel::mm::layout::LowGuardEnd},
                kernel::mm::layout::UserEnd - kernel::mm::layout::LowGuardEnd},
            .access = kernel::mm::AccessMask::of(
                kernel::mm::Access::Read,
                kernel::mm::Access::Write,
                kernel::mm::Access::Execute),
            .types = kernel::mm::MemoryTypes::of(kernel::mm::MemoryType::Normal),
        };
    }

    [[nodiscard]] static constexpr auto context() noexcept -> kernel::mm::VmContext {
        return kernel::mm::VmContext{.local = kernel::CpuId{0}};
    }

    [[nodiscard]] auto space() noexcept -> kernel::mm::VSpace& { return space_.get(); }
    [[nodiscard]] auto memory() noexcept -> kernel::mm::MemoryObject& {
        return memory_.get();
    }
    [[nodiscard]] auto memory_ref() noexcept
        -> libk::Expected<kernel::object::ObjectRef, kernel::object::ObjectError> {
        return memory_.ref();
    }
    [[nodiscard]] auto space_ref() noexcept
        -> libk::Expected<kernel::object::ObjectRef, kernel::object::ObjectError> {
        return space_.ref();
    }
    [[nodiscard]] auto cspace_ref() noexcept
        -> libk::Expected<kernel::object::ObjectRef, kernel::object::ObjectError> {
        return cspace_.ref();
    }
    [[nodiscard]] auto cspace() noexcept -> kernel::cap::CSpace& {
        return cspace_.get();
    }
    [[nodiscard]] auto retire_space() noexcept -> bool {
        return space_.retire();
    }
    [[nodiscard]] auto retire_cspace() noexcept -> bool {
        return cspace_.retire();
    }
    [[nodiscard]] auto retire_memory() noexcept -> bool {
        return memory_.retire();
    }
    [[nodiscard]] auto pmm() noexcept -> kernel::mm::Pmm& { return *vspace_test_pmm; }
    [[nodiscard]] auto aliases() noexcept -> kernel::mm::PhysicalAliasRegistry& {
        return vspace_test_kernel->aliases();
    }
    void run_work() noexcept {
        while (vspace_test_work->run(context(), 8)) {}
    }

private:
    void reset() noexcept {
        if (cspace_) {
            KASSERT(cspace_.retire());
            cspace_.reset();
        }
        if (space_) {
            KASSERT(space_.retire());
            while (space_->state() != kernel::mm::VSpaceState::Quiescent) {
                auto serviced = space_->service(context());
                KASSERT(serviced);
            }
            while (vspace_test_work->run(context(), 8)) {}
            space_.reset();
        }
        if (memory_) {
            if (memory_->state() == kernel::mm::MemoryState::Live) {
                KASSERT(memory_.retire());
            } else {
                KASSERT(memory_->state() == kernel::mm::MemoryState::Stopping
                    || memory_->state() == kernel::mm::MemoryState::Retired);
            }
            memory_.reset();
        }
        if (vspace_test_objects) {
            vspace_test_objects->drain_reclaim();
        }
        vspace_test_objects.reset();
        vspace_test_work.reset();
        vspace_test_kernel.reset();
        vspace_test_pmm.reset();
        vspace_test_direct.reset();
        vspace_test_map.reset();
    }

    kernel::object::ObjectStore::VSpaceHold space_{};
    kernel::object::ObjectStore::MemoryHold memory_{};
    kernel::object::ObjectStore::CSpaceHold cspace_{};
};

bool test_semantic_map_protect_and_arbitrary_unmap(
    const TestContext&) noexcept {
    VSpaceFixture fixture{};
    if (!fixture.initialize(true)) {
        return false;
    }
    const kernel::mm::VirtRange whole{kernel::mm::VirtAddr{0x20000}, 4 * kernel::mm::page_size};
    auto mapped = fixture.map_kernel(
        whole,
        kernel::mm::ObjectRange{0, 4},
        kernel::mm::AccessMask::of(kernel::mm::Access::Read, kernel::mm::Access::Write));
    if (!mapped || mapped.value().status != kernel::mm::VmStatus::Complete
        || fixture.aliases().active_pages() != 4) {
        return false;
    }
    auto info = fixture.space().inspect(mapped.value().mapping);
    if (!info || info.value().range != whole
        || info.value().source != kernel::mm::AuthoritySource::Kernel) {
        return false;
    }
    const kernel::mm::VirtRange middle{
        kernel::mm::VirtAddr{0x21000}, 2 * kernel::mm::page_size};
    auto protected_result = fixture.space().protect_kernel(
        fixture.context(),
        fixture.space().root_key(),
        middle,
        kernel::mm::AccessMask::of(kernel::mm::Access::Read));
    if (!protected_result
        || protected_result.value() != kernel::mm::VmStatus::Complete) {
        return false;
    }
    auto denied = fixture.space().fault(
        fixture.context(), kernel::mm::VirtAddr{0x21000}, kernel::mm::Access::Write);
    auto ready = fixture.space().fault(
        fixture.context(), kernel::mm::VirtAddr{0x21000}, kernel::mm::Access::Read);
    if (!denied || denied.value().kind != kernel::mm::FaultKind::AccessDenied
        || !ready || ready.value().kind != kernel::mm::FaultKind::Ready) {
        return false;
    }
    auto unmapped = fixture.space().unmap_kernel(
        fixture.context(), fixture.space().root_key(), whole);
    auto absent = fixture.space().fault(
        fixture.context(), kernel::mm::VirtAddr{0x22000}, kernel::mm::Access::Read);
    return unmapped && unmapped.value() == kernel::mm::VmStatus::Complete
        && fixture.aliases().active_pages() == 0
        && absent && absent.value().kind == kernel::mm::FaultKind::NoMapping;
}

bool test_lazy_fault_materialization_and_split_unmap(
    const TestContext&) noexcept {
    VSpaceFixture fixture{};
    if (!fixture.initialize(false, 2)) {
        return false;
    }
    const kernel::mm::VirtRange whole{kernel::mm::VirtAddr{0x40000}, 2 * kernel::mm::page_size};
    auto mapped = fixture.map_kernel(
        whole,
        kernel::mm::ObjectRange{0, 2},
        kernel::mm::AccessMask::of(kernel::mm::Access::Read, kernel::mm::Access::Write));
    if (!mapped || fixture.aliases().active_pages() != 0) {
        return false;
    }
    auto materialized = fixture.space().fault(
        fixture.context(), kernel::mm::VirtAddr{0x40020}, kernel::mm::Access::Read);
    if (!materialized
        || materialized.value().kind != kernel::mm::FaultKind::Materialized
        || materialized.value().status != kernel::mm::VmStatus::Complete
        || fixture.aliases().active_pages() != 1) {
        return false;
    }
    auto unmapped = fixture.space().unmap_kernel(
        fixture.context(),
        fixture.space().root_key(),
        kernel::mm::VirtRange{kernel::mm::VirtAddr{0x40000}, kernel::mm::page_size});
    auto second = fixture.space().fault(
        fixture.context(), kernel::mm::VirtAddr{0x41000}, kernel::mm::Access::Write);
    return unmapped && unmapped.value() == kernel::mm::VmStatus::Complete
        && second && second.value().kind == kernel::mm::FaultKind::Materialized
        && fixture.aliases().active_pages() == 1;
}

bool test_capability_mapping_revokes_after_hardware_retirement(
    const TestContext&) noexcept {
    VSpaceFixture fixture{};
    if (!fixture.initialize(true, 1)) {
        return false;
    }
    kernel::cap::GrantGraph graph{fixture.pmm()};
    kernel::cap::CSpace cspace{fixture.pmm()};
    auto reference = fixture.memory_ref();
    if (!reference) {
        return false;
    }
    const auto memory_authority = fixture.memory_authority(1);
    auto grant = graph.create_root(
        libk::move(reference).value(),
        kernel::cap::GrantCeiling{
            kernel::cap::Rights::of(
                kernel::cap::Right::Map,
                kernel::cap::Right::Inspect),
            memory_authority});
    if (!grant) {
        return false;
    }
    auto inserted = cspace.insert(
        libk::move(grant).value(),
        kernel::cap::CapView{
            kernel::cap::Rights::of(kernel::cap::Right::Map),
            memory_authority});
    if (!inserted) {
        return false;
    }
    kernel::cap::GrantKey key{};
    kernel::cap::GrantRevoke completion{};
    {
        auto resolved = cspace.resolve<kernel::mm::MemoryObject>(
            inserted.value(),
            kernel::cap::Rights::of(kernel::cap::Right::Map));
        if (!resolved) {
            return false;
        }
        key = resolved.value().grant();
        auto mapped = fixture.space().map(
            fixture.context(),
            fixture.root_authority(),
            kernel::mm::MapRequest{
                kernel::mm::VirtRange{kernel::mm::VirtAddr{0x60000}, kernel::mm::page_size},
                kernel::mm::ObjectRange{0, 1},
                kernel::mm::AccessMask::of(kernel::mm::Access::Read)},
            resolved.value());
        if (!mapped || fixture.aliases().active_pages() != 1
            || !graph.invalidate(key, completion)
            || completion.complete()) {
            return false;
        }
        fixture.run_work();
        if (fixture.aliases().active_pages() != 0
            || completion.complete()) {
            return false;
        }
    }
    const bool revoked = completion.complete();
    const bool closed = static_cast<bool>(cspace.close(inserted.value()));
    cspace.retire();
    return revoked && closed && graph.live_count() == 0;
}

bool test_child_region_and_capability_publish_together(
    const TestContext&) noexcept {
    VSpaceFixture fixture{};
    if (!fixture.initialize(true, 1)) {
        return false;
    }
    kernel::cap::GrantGraph graph{fixture.pmm()};
    kernel::cap::CSpace cspace{fixture.pmm()};
    auto reference = fixture.space_ref();
    if (!reference) {
        return false;
    }
    const auto root_authority = fixture.root_authority();
    const auto root_rights = kernel::cap::Rights::of(
        kernel::cap::Right::CreateRegion,
        kernel::cap::Right::Map,
        kernel::cap::Right::Inspect);
    auto grant = graph.create_root(
        libk::move(reference).value(),
        kernel::cap::GrantCeiling{root_rights, root_authority});
    if (!grant) {
        return false;
    }
    auto root = cspace.insert(
        libk::move(grant).value(),
        kernel::cap::CapView{root_rights, root_authority});
    if (!root) {
        return false;
    }

    const kernel::mm::VirtRange child_range{
        kernel::mm::VirtAddr{0x80000}, 4 * kernel::mm::page_size};
    const kernel::mm::RegionPolicy child_policy{
        .access = kernel::mm::AccessMask::of(kernel::mm::Access::Read, kernel::mm::Access::Write),
        .types = kernel::mm::MemoryTypes::of(kernel::mm::MemoryType::Normal),
    };
    kernel::mm::RegionCapResult child{};
    {
        auto resolved = cspace.resolve<kernel::mm::VSpace>(
            root.value(),
            kernel::cap::Rights::of(kernel::cap::Right::CreateRegion));
        if (!resolved) {
            return false;
        }
        auto created = fixture.space().create_region(
            resolved.value(),
            cspace,
            child_range,
            child_policy,
            kernel::cap::Rights::of(
                kernel::cap::Right::Map,
                kernel::cap::Right::Inspect));
        if (!created) {
            return false;
        }
        child = created.value();
    }
    bool coherent{};
    {
        auto resolved_child = cspace.resolve<kernel::mm::VSpace>(
            child.capability,
            kernel::cap::Rights::of(kernel::cap::Right::Map));
        if (!resolved_child) {
            return false;
        }
        const kernel::cap::EffectiveAuthority effective =
            resolved_child.value().authority();
        const auto* authority = libk::get_if<kernel::cap::VSpaceAuthority>(
            &effective.data);
        coherent = authority != nullptr
            && authority->region == child.region
            && authority->range == child_range
            && authority->access == child_policy.access
            && authority->types == child_policy.types;
    }
    const bool closed_child = static_cast<bool>(cspace.close(child.capability));
    const bool closed_root = static_cast<bool>(cspace.close(root.value()));
    cspace.retire();
    return coherent && closed_child && closed_root && graph.live_count() == 0;
}

bool test_memory_retire_invalidates_mapping_projection(
    const TestContext&) noexcept {
    VSpaceFixture fixture{};
    if (!fixture.initialize(true, 1)) {
        return false;
    }
    const kernel::mm::VirtRange range{kernel::mm::VirtAddr{0xa0000}, kernel::mm::page_size};
    auto mapped = fixture.map_kernel(
        range,
        kernel::mm::ObjectRange{0, 1},
        kernel::mm::AccessMask::of(kernel::mm::Access::Read));
    if (!mapped || fixture.aliases().active_pages() != 1
        || fixture.memory().attachment_count() != 1
        || !fixture.retire_memory()
        || fixture.memory().state() != kernel::mm::MemoryState::Stopping) {
        return false;
    }
    auto serviced = fixture.space().service(fixture.context());
    auto absent = fixture.space().fault(
        fixture.context(), range.base(), kernel::mm::Access::Read);
    return serviced
        && serviced.value() == kernel::mm::VSpaceServiceState::Settled
        && fixture.aliases().active_pages() == 0
        && fixture.memory().attachment_count() == 0
        && fixture.memory().state() == kernel::mm::MemoryState::Retired
        && absent && absent.value().kind == kernel::mm::FaultKind::NoMapping;
}

bool test_execution_binding_blocks_root_retirement(
    const TestContext&) noexcept {
    VSpaceFixture fixture{};
    if (!fixture.initialize(false, 1)) {
        return false;
    }
    {
        auto vspace = fixture.space_ref();
        auto cspace = fixture.cspace_ref();
        if (!vspace || !cspace) {
            return false;
        }
        auto binding = kernel::ExecutionBinding::user(
            libk::move(vspace).value(),
            libk::move(cspace).value());
        if (!binding || !binding.value().user_bound()
            || binding.value().vspace() != &fixture.space()
            || binding.value().cspace() != &fixture.cspace()
            || fixture.space().binding_count() != 1
            || fixture.cspace().binding_count() != 1
            || fixture.retire_space()
            || fixture.retire_cspace()) {
            return false;
        }
    }
    return fixture.space().state() == kernel::mm::VSpaceState::Live
        && fixture.space().binding_count() == 0
        && fixture.cspace().binding_count() == 0;
}

bool test_ipc_binding_is_validated_and_invalidated_with_mapping(
    const TestContext&) noexcept {
    VSpaceFixture fixture{};
    if (!fixture.initialize(true, 2)) {
        return false;
    }
    const kernel::mm::VirtRange whole{
        kernel::mm::VirtAddr{0xc0000}, 2 * kernel::mm::page_size};
    auto mapped = fixture.map_kernel(
        whole,
        kernel::mm::ObjectRange{0, 2},
        kernel::mm::AccessMask::of(kernel::mm::Access::Read, kernel::mm::Access::Write));
    if (!mapped) {
        return false;
    }
    bool invalidated{};
    {
        auto vspace = fixture.space_ref();
        auto cspace = fixture.cspace_ref();
        auto memory = fixture.memory_ref();
        if (!vspace || !cspace || !memory) {
            return false;
        }
        kernel::mm::UserViewRequest request{
            .memory = libk::move(memory).value(),
            .object = kernel::mm::ObjectRange{0, 1},
            .virtual_range = kernel::mm::VirtRange{
                whole.base(), kernel::mm::page_size},
            .access = kernel::mm::AccessMask::of(
                kernel::mm::Access::Read, kernel::mm::Access::Write),
        };
        auto binding = kernel::ExecutionBinding::user(
            libk::move(vspace).value(),
            libk::move(cspace).value(),
            kernel::FaultRoute::Terminate,
            libk::optional<kernel::mm::UserViewRequest>{libk::move(request)});
        if (!binding || binding.value().ipc_buffer() == nullptr
            || !binding.value().ipc_buffer()->valid()) {
            return false;
        }
        auto protected_result = fixture.space().protect_kernel(
            fixture.context(),
            fixture.space().root_key(),
            kernel::mm::VirtRange{whole.base(), kernel::mm::page_size},
            kernel::mm::AccessMask::of(kernel::mm::Access::Read));
        auto unmapped = fixture.space().unmap_kernel(
            fixture.context(),
            fixture.space().root_key(),
            kernel::mm::VirtRange{whole.base(), kernel::mm::page_size});
        if (protected_result
            || protected_result.error() != kernel::mm::VSpaceError::Busy
            || unmapped || unmapped.error() != kernel::mm::VSpaceError::Busy
            || !fixture.retire_memory()) {
            return false;
        }
        auto serviced = fixture.space().service(fixture.context());
        invalidated = serviced
            && serviced.value() == kernel::mm::VSpaceServiceState::Settled
            && !binding.value().ipc_buffer()->valid()
            && fixture.aliases().active_pages() == 0
            && fixture.memory().state() == kernel::mm::MemoryState::Retired;
    }
    return invalidated
        && fixture.space().binding_count() == 0
        && fixture.cspace().binding_count() == 0;
}

bool test_sponsored_table_capacity_follows_retirement(
    const TestContext&) noexcept {
    VSpaceFixture fixture{};
    if (!fixture.initialize(true, 1)) {
        return false;
    }

    constexpr kernel::resource::Budget limit{
        .memory = 16 * kernel::mm::page_size,
    };
    auto pending_pool = vspace_test_objects->create_resource(limit);
    if (!pending_pool) {
        return false;
    }
    auto pool = libk::move(pending_pool).value().publish();
    auto pool_ref = pool.ref();
    if (!pool_ref) {
        return false;
    }
    constexpr auto fixed =
        kernel::resource::Traits<kernel::mm::VSpace>::fixed();
    auto reserved = pool->reserve(libk::move(pool_ref).value(), fixed);
    if (!reserved) {
        return false;
    }
    auto pending_space = vspace_test_objects->create_vspace_sponsored(
        libk::move(reserved).value(), *vspace_test_kernel);
    if (!pending_space) {
        return false;
    }
    auto space = libk::move(pending_space).value().publish();

    const kernel::resource::Budget root_baseline{
        .memory = limit.memory - fixed.memory
            - 2 * kernel::mm::page_size,
    };
    if (pool->available() != root_baseline
        || pool->sponsorship_count() != 2) {
        return false;
    }

    auto memory_ref = fixture.memory_ref();
    if (!memory_ref) {
        return false;
    }
    const kernel::mm::VirtRange range{
        kernel::mm::VirtAddr{0xe0000},
        kernel::mm::page_size,
    };
    auto mapped = space->map_kernel(
        fixture.context(),
        space->root_key(),
        kernel::mm::MapRequest{
            range,
            kernel::mm::ObjectRange{0, 1},
            kernel::mm::AccessMask::of(
                kernel::mm::Access::Read,
                kernel::mm::Access::Write),
        },
        libk::move(memory_ref).value(),
        fixture.memory(),
        fixture.memory_authority(1));
    if (!mapped || mapped.value().status != kernel::mm::VmStatus::Complete
        || pool->available().memory >= root_baseline.memory) {
        return false;
    }

    auto unmapped = space->unmap_kernel(
        fixture.context(), space->root_key(), range);
    if (!unmapped || unmapped.value() != kernel::mm::VmStatus::Complete
        || pool->available() != root_baseline
        || pool->sponsorship_count() != 2) {
        return false;
    }

    if (!space.retire()) {
        return false;
    }
    while (space->state() != kernel::mm::VSpaceState::Quiescent) {
        if (!space->service(fixture.context())) {
            return false;
        }
    }
    while (vspace_test_work->run(fixture.context(), 8)) {}
    space.reset();
    vspace_test_objects->drain_reclaim();
    if (pool->available() != limit || pool->sponsorship_count() != 0
        || pool->close() != kernel::resource::PoolState::Closed
        || !pool.retire()) {
        return false;
    }
    pool.reset();
    vspace_test_objects->drain_reclaim();
    return fixture.pmm().verify_invariants();
}

} // namespace

void register_vspace_tests(TestRegistry& registry) noexcept {
    (void)registry.add(
        "vspace",
        "semantic map, protect and arbitrary unmap share one layout truth",
        test_semantic_map_protect_and_arbitrary_unmap);
    (void)registry.add(
        "vspace",
        "lazy fault materialization survives mapping split",
        test_lazy_fault_materialization_and_split_unmap);
    (void)registry.add(
        "vspace",
        "capability revoke waits for PTE and alias retirement",
        test_capability_mapping_revokes_after_hardware_retirement);
    (void)registry.add(
        "vspace",
        "child Region and capability publish in one transaction",
        test_child_region_and_capability_publish_together);
    (void)registry.add(
        "vspace",
        "Memory retirement invalidates mapping and hardware projection",
        test_memory_retire_invalidates_mapping_projection);
    (void)registry.add(
        "vspace",
        "ExecutionBinding blocks retirement of effective roots",
        test_execution_binding_blocks_root_retirement);
    (void)registry.add(
        "vspace",
        "IPC binding blocks normal edits and follows strong invalidation",
        test_ipc_binding_is_validated_and_invalidated_with_mapping);
    (void)registry.add(
        "vspace",
        "sponsored table capacity follows physical retirement",
        test_sponsored_table_capacity_follows_retirement);
}
