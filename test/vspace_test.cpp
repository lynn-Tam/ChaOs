#include <expected>
#include <optional>
#include <object/pool.hpp>
#include <test/test.hpp>

#include <mm/table.hpp>
#include <cap/cspace.hpp>
#include <cap/grant.hpp>
#include <libk/manual_lifetime.hpp>
#include <libk/noncopyable.hpp>
#include <libk/scope_guard.hpp>
#include <utility>
#include <mm/kspace.hpp>
#include <mm/vspace.hpp>
#include <mm/mem.hpp>
#include <object/group.hpp>
#include <mm/pager.hpp>
#include <boot/info.hpp>
#include <task/env.hpp>

//Confirmatory experiment.
// Exit condition: retained as the permanent semantic/PTE/revocation contract
// once D5-D6 are integrated into the runtime dispatcher and syscall tests.

namespace {

constexpr usize vspace_test_page_count = 384;
alignas(mm::page_size) byte
    vspace_test_ram[vspace_test_page_count * mm::page_size]{};
constinit libk::ManualLifetime<mm::RegionList> vspace_test_map{};
constinit libk::ManualLifetime<mm::Pmm> vspace_test_pmm{};
constinit libk::ManualLifetime<mm::KSpace> vspace_test_kernel{};
constinit libk::ManualLifetime<object::store<mm::VSpace, mm::Mem, Pager>> vspace_test_mm{};
constinit libk::ManualLifetime<WorkQueue> vspace_work{};
constinit libk::ManualLifetime<object::pool<object::group>> vspace_test_groups{};
constinit libk::ManualLifetime<object::pool<cap::CSpace>> vspace_test_cspaces{};



class VSpaceFixture final : private libk::noncopyable_nonmovable {
public:
    VSpaceFixture() noexcept = default;
    ~VSpaceFixture() noexcept { reset(); }

    [[nodiscard]] auto initialize(bool eager, usize pages = 4) noexcept
        -> bool {
        reset();
        (void)vspace_work.emplace();
        const auto physical = boot_layout.phys(mm::Virt{
            reinterpret_cast<usize>(vspace_test_ram)});
        if (!physical) {
            return false;
        }
        const auto first = mm::Page::from_base(*physical);
        if (!first) {
            return false;
        }
        auto& map = vspace_test_map.emplace();
        // The private allocator owns only test RAM; its kernel root also maps
        // the real linked code and secondary entry, all reserved resources.
        const auto test_end = first->checked_add(vspace_test_page_count);
        const auto image = boot_layout.kernel.pages();
        const auto low = boot_layout.entry.pages().base();
        if (!test_end || !map.try_emplace_back(mm::Region{
                mm::Pages{low, first->raw() - low.raw()}, mm::Region::Kind::Kernel}) ||
            !map.try_emplace_back(mm::Region{
                mm::Pages{*first, vspace_test_page_count}, mm::Region::Kind::Ram}))
            return false;
        // The test arena may be the final BSS allocation: no empty reservation.
        if (*test_end < *image.limit() && !map.try_emplace_back(mm::Region{
                mm::Pages{*test_end, image.limit()->raw() - test_end->raw()}, mm::Region::Kind::Kernel}))
            return false;
        auto initialized = mm::Pmm::initialize_in(
                vspace_test_pmm, std::move(map), mm::Pmm::Window{
                .pa = mm::Phys{0},
                .va = mm::Virt{mm::DirectBegin},
                .size = mm::DirectSize,
            });
        if (!initialized) {
            reset();
            return false;
        }
        vspace_test_map.reset();
        auto root = kernel_root(*vspace_test_pmm);
        if (!root) { reset(); return false; }
        (void)vspace_test_kernel.emplace(*vspace_test_pmm, std::move(*root), boot_layout.va);
        auto& work = *vspace_work;

        auto& mm = vspace_test_mm.emplace(*vspace_test_pmm, *vspace_work);
        (void)vspace_test_groups.emplace(*vspace_test_pmm, *vspace_work);
        (void)vspace_test_cspaces.emplace(*vspace_test_pmm, *vspace_work);
        auto memory = mm.get<mm::Mem>().create({}, *vspace_test_pmm, pages * mm::page_size, mm::AnonCfg{
                .perms = mm::Perms::of(
                    mm::Perm::Read, mm::Perm::Write),
                .eager = eager,
            });
        if (!memory) {
            reset();
            return false;
        }
        memory_ = std::move(memory).value().publish();
        auto space = mm.get<mm::VSpace>().create(resource::Reservation{}, *vspace_test_pmm, *vspace_test_kernel, work);
        if (!space) {
            reset();
            return false;
        }
        space_ = std::move(space).value().publish();
        auto cspace = vspace_test_cspaces->create(*vspace_test_pmm);
        if (!cspace) {
            reset();
            return false;
        }
        cspace_ = std::move(cspace).value().publish();
        return true;
    }

    [[nodiscard]] auto map(
        mm::VRange range,
        mm::ObjectRange object,
        mm::Perms access) noexcept
        -> std::expected<mm::MapResult, mm::VSpaceError> {
        auto reference = memory_.erase();
        if (!reference) {
            return std::unexpected(mm::VSpaceError::InvalidState);
        }
        return space_->map(
            context(),
            mm::MapReq{range, object, access},
            std::move(reference).value(),
            memory_.get(),
            memory_authority(memory_.get().page_count()));
    }

    [[nodiscard]] static auto memory_authority(usize pages) noexcept
        -> cap::MemLimit {
        return cap::MemLimit{
            .range = mm::ObjectRange{0, pages},
            .perms = mm::Perms::of(
                mm::Perm::Read, mm::Perm::Write),
        };
    }

    [[nodiscard]] auto root_authority() noexcept
        -> cap::VmLimit {
        return cap::VmLimit{
            .range = mm::VRange{
                mm::Virt{mm::UserBegin},
                mm::UserEnd - mm::UserBegin},
            .perms = mm::Perms::of(
                mm::Perm::Read,
                mm::Perm::Write,
                mm::Perm::Execute),
        };
    }

    [[nodiscard]] static constexpr auto context() noexcept -> mm::VmCtx {
        return mm::VmCtx{.local = CpuId{0}};
    }

    [[nodiscard]] auto space() noexcept -> mm::VSpace& { return space_.get(); }
    [[nodiscard]] auto memory() noexcept -> mm::Mem& {
        return memory_.get();
    }
    [[nodiscard]] auto memory_ref() noexcept
        -> std::expected<object::ref<>, object::error> {
        return memory_.erase();
    }
    [[nodiscard]] auto space_ref() noexcept
        -> std::expected<object::ref<>, object::error> {
        return space_.erase();
    }
    [[nodiscard]] auto cspace_ref() noexcept
        -> std::expected<object::ref<>, object::error> {
        return cspace_.erase();
    }
    [[nodiscard]] auto cspace() noexcept -> cap::CSpace& {
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
    [[nodiscard]] auto pmm() noexcept -> mm::Pmm& { return *vspace_test_pmm; }

    void run_work() noexcept {
        while (vspace_work->run(8)) {}
    }

private:
    void reset() noexcept {
        if (cspace_) {
            libk_assert(cspace_.retire());
            cspace_.reset();
        }
        if (space_) {
            libk_assert(space_.retire());
            run_work();
            libk_assert(space_->state() == mm::VSpaceState::Quiescent);
            space_.reset();
        }
        if (memory_) {
            if (memory_->state() == mm::MemState::Live) {
                libk_assert(memory_.retire());
            } else {
                libk_assert(memory_->state() == mm::MemState::Stopping
                    || memory_->state() == mm::MemState::Retired);
            }
            memory_.reset();
        }
        if (vspace_test_mm) {
            while (vspace_work->run()) {}


        }
        vspace_test_cspaces.reset();
        vspace_test_mm.reset();
        vspace_test_groups.reset();

        vspace_test_kernel.reset();
        vspace_work.reset();
        vspace_test_pmm.reset();
        vspace_test_map.reset();
    }

    object::ref<mm::VSpace> space_{};
    object::ref<mm::Mem> memory_{};
    object::ref<cap::CSpace> cspace_{};
};

bool test_semantic_map_protect_and_arbitrary_unmap(
    const TestContext&) noexcept {
    VSpaceFixture fixture{};
    if (!fixture.initialize(true)) {
        return false;
    }
    const mm::VRange whole{mm::Virt{0x20000}, 4 * mm::page_size};
    auto mapped = fixture.map(
        whole,
        mm::ObjectRange{0, 4},
        mm::Perms::of(mm::Perm::Read, mm::Perm::Write));
    if (!mapped || mapped.value().status != mm::VmStatus::Complete) {
        return false;
    }
    auto info = fixture.space().inspect(mapped.value().mapping);
    if (!info || info.value().range != whole) {
        return false;
    }
    const mm::VRange middle{
        mm::Virt{0x21000}, 2 * mm::page_size};
    auto protected_result = fixture.space().protect(
        fixture.context(),
        middle,
        mm::Perms::of(mm::Perm::Read));
    if (!protected_result
        || protected_result.value() != mm::VmStatus::Complete) {
        return false;
    }
    auto denied = fixture.space().fault(
        fixture.context(), mm::Virt{0x21000}, mm::Perm::Write);
    auto ready = fixture.space().fault(
        fixture.context(), mm::Virt{0x21000}, mm::Perm::Read);
    if (!denied || denied.value().kind != mm::FaultKind::AccessDenied
        || !ready || ready.value().kind != mm::FaultKind::Ready) {
        return false;
    }
    auto unmapped = fixture.space().unmap(
        fixture.context(), whole);
    auto absent = fixture.space().fault(
        fixture.context(), mm::Virt{0x22000}, mm::Perm::Read);
    return unmapped && unmapped.value() == mm::VmStatus::Complete
        && absent && absent.value().kind == mm::FaultKind::NoMapping;
}

bool test_lazy_fault_materialization_and_split_unmap(
    const TestContext&) noexcept {
    VSpaceFixture fixture{};
    if (!fixture.initialize(false, 2)) {
        return false;
    }
    const mm::VRange whole{mm::Virt{0x40000}, 2 * mm::page_size};
    auto mapped = fixture.map(
        whole,
        mm::ObjectRange{0, 2},
        mm::Perms::of(mm::Perm::Read, mm::Perm::Write));
    if (!mapped) {
        return false;
    }
    auto materialized = fixture.space().fault(
        fixture.context(), mm::Virt{0x40020}, mm::Perm::Read);
    if (!materialized
        || materialized.value().kind != mm::FaultKind::Materialized
        || materialized.value().status != mm::VmStatus::Complete) {
        return false;
    }
    auto unmapped = fixture.space().unmap(
        fixture.context(),
        mm::VRange{mm::Virt{0x40000}, mm::page_size});
    auto second = fixture.space().fault(
        fixture.context(), mm::Virt{0x41000}, mm::Perm::Write);
    return unmapped && unmapped.value() == mm::VmStatus::Complete
        && second && second.value().kind == mm::FaultKind::Materialized;
}

bool test_mapping_during_page_in_joins_existing_request(const TestContext&) noexcept {
    using namespace mm;
    VSpaceFixture fixture;
    if (!fixture.initialize(false)) return false;
    object::pool<mm::Mem> memories{fixture.pmm(), *vspace_work};
    auto pending_pager = vspace_test_mm->get<Pager>().create();
    if (!pending_pager) return false;
    auto pager = std::move(pending_pager).value().publish();
    auto close_pager = libk::on_scope_exit([&]() noexcept {
        libk_assert(pager.retire());
        pager.reset();
        while (vspace_work->run()) {}


    });
    auto reference = pager.erase();
    const auto access = Perms::of(Perm::Read);
    if (!reference) return false;
    auto pending = memories.create(fixture.pmm(), page_size, mm::PagedCfg{std::move(*reference), access});
    if (!pending) return false;
    auto memory = std::move(pending).value().publish();
    const VRange ranges[]{ {Virt{0x40000}, page_size}, {Virt{0x50000}, page_size} };
    auto cleanup = libk::on_scope_exit([&]() noexcept {
        for (const auto range : ranges)
            (void)fixture.space().unmap(fixture.context(), range);
        libk_assert(memory.retire());
        memory.reset();

        fixture.run_work();
    });
    for (usize index = 0; index < 2; ++index) {
        // The second mapping is admitted while the first fault is pending.
        auto ref = memory.erase();
        if (!ref) return false;
        const auto mapped = fixture.space().map(fixture.context(), MapReq{ranges[index], {0, 1}, access}, std::move(ref).value(), memory.get(),
            fixture.memory_authority(1));
        if (!mapped || mapped.value().status != VmStatus::Complete) return false;
        const auto fault = fixture.space().fault(fixture.context(), ranges[index].base(), Perm::Read);
        if (!fault || fault.value().kind != FaultKind::Pending
            || memory->query(0).value() != ContentState::Busy) return false;
    }

    if (pager->pending() != 1) return false;
    const auto request = pager->claim();
    auto page = fixture.pmm().allocate_page();
    if (!request || !page) return false;
    if (!memory->supply(pager.get(), request.value().id, std::move(page).value())) return false;

    for (usize index = 0; index < 2; ++index) {
        const auto fault = fixture.space().fault(fixture.context(), ranges[index].base(), Perm::Read);
        if (!fault || fault.value().kind != FaultKind::Materialized) return false;
    }
    return pager->pending() == 0;
}

bool test_capability_mapping_revokes_after_hardware_retirement(
    const TestContext&) noexcept {
    VSpaceFixture fixture{};
    if (!fixture.initialize(true, 1)) {
        return false;
    }
    cap::Graph graph{fixture.pmm(), *vspace_work};
    libk::scope_exit drain{[]() noexcept { while (vspace_work->run()) {} }};
    cap::CSpace cspace{fixture.pmm()};
    auto reference = fixture.memory_ref();
    if (!reference) {
        return false;
    }
    const auto memory_authority = fixture.memory_authority(1);
    auto grant = graph.create_root(
        std::move(reference).value(),
        cap::View{
            cap::Rights::of(
                cap::Right::Map,
                cap::Right::Inspect),
            memory_authority});
    if (!grant) {
        return false;
    }
    auto inserted = cspace.insert(
        std::move(grant).value(),
        cap::View{
            cap::Rights::of(cap::Right::Map),
            memory_authority});
    if (!inserted) {
        return false;
    }
    cap::GrantKey key{};
    cap::GrantRevoke completion{};
    libk::scope_exit drained{[&]() noexcept { fixture.run_work(); }};
    {
        auto resolved = cspace.resolve<mm::Mem>(
            inserted.value(),
            cap::Rights::of(cap::Right::Map));
        if (!resolved) {
            return false;
        }
        key = resolved.value().grant();
        auto mapped = fixture.space().map(
            fixture.context(),
            fixture.root_authority(),
            mm::MapReq{
                mm::VRange{mm::Virt{0x60000}, mm::page_size},
                mm::ObjectRange{0, 1},
                mm::Perms::of(mm::Perm::Read)},
            resolved.value());
        if (!mapped
            || !graph.invalidate(key, completion)
            || completion.complete()) {
            return false;
        }
        fixture.run_work();
        if (completion.complete()) {
            return false;
        }
    }
    fixture.run_work();
    const bool revoked = completion.complete();
    const bool closed = static_cast<bool>(cspace.close(inserted.value()));
    cspace.retire();
    fixture.run_work();
    return revoked && closed && graph.live_count() == 0;
}

bool test_vm_slice_is_capability_only(const TestContext&) noexcept {
    VSpaceFixture fixture{};
    if (!fixture.initialize(true,1)) return false;
    cap::Graph graph{fixture.pmm(), *vspace_work};
    libk::scope_exit drain{[]() noexcept { while (vspace_work->run()) {} }}; cap::CSpace caps{fixture.pmm()};
    auto ref = fixture.space_ref(); if (!ref) return false;
    const auto full = fixture.root_authority();
    auto rights = cap::Rights::of(cap::Right::Delegate,cap::Right::Map,cap::Right::Destroy);
    auto grant = graph.create_root(std::move(*ref),cap::View{rights,full});
    if (!grant) return false;
    auto root = caps.insert(std::move(*grant),cap::View{rights,full}); if (!root) return false;
    auto clipped = full; clipped.range = mm::VRange{mm::Virt{0x80000},4*mm::page_size};
    const cap::View view{cap::Rights::of(cap::Right::Map,cap::Right::Destroy),clipped};
    auto slice = caps.transfer(*root, caps, cap::XferOp::Derive, view);
    bool ok = false;
    if (slice) {
        auto hold = caps.resolve<mm::VSpace>(*slice,cap::Rights::of(cap::Right::Map));
        ok = hold && std::get<cap::VmLimit>(hold->view().data) == clipped
            && !fixture.space().can_destroy_object(clipped) && fixture.space().can_destroy_object(full);
        hold = std::unexpected(cap::CSpaceError::InvalidHandle);
        auto denied = caps.destroy(*slice);
        ok = ok && !denied && denied.error() == cap::CSpaceError::Denied;
        static_cast<void>(caps.close(*slice));
    }
    static_cast<void>(caps.close(*root)); caps.retire();
    fixture.run_work();
    return ok && graph.live_count() == 0;
}

bool test_memory_retire_invalidates_mapping_projection(
    const TestContext&) noexcept {
    VSpaceFixture fixture{};
    if (!fixture.initialize(true, 1)) {
        return false;
    }
    const mm::VRange range{mm::Virt{0xa0000}, mm::page_size};
    auto mapped = fixture.map(
        range,
        mm::ObjectRange{0, 1},
        mm::Perms::of(mm::Perm::Read));
    if (!mapped
        || fixture.memory().attachment_count() != 1
        || !fixture.retire_memory()
        || fixture.memory().state() != mm::MemState::Stopping) {
        return false;
    }
    fixture.run_work();
    auto absent = fixture.space().fault(
        fixture.context(), range.base(), mm::Perm::Read);
    return !fixture.space().pending()
        && fixture.memory().attachment_count() == 0
        && fixture.memory().state() == mm::MemState::Retired
        && absent && absent.value().kind == mm::FaultKind::NoMapping;
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
        auto binding = Env::user(
            std::move(vspace).value(),
            std::move(cspace).value());
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
    return fixture.space().state() == mm::VSpaceState::Live
        && fixture.space().binding_count() == 0
        && fixture.cspace().binding_count() == 0;
}

bool test_ipc_binding_is_validated_and_invalidated_with_mapping(
    const TestContext&) noexcept {
    VSpaceFixture fixture{};
    if (!fixture.initialize(true, 2)) {
        return false;
    }
    const mm::VRange whole{
        mm::Virt{0xc0000}, 2 * mm::page_size};
    auto mapped = fixture.map(
        whole,
        mm::ObjectRange{0, 2},
        mm::Perms::of(mm::Perm::Read, mm::Perm::Write));
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
        auto ipc = ipc::Buffer::bind(
            fixture.pmm(),
            fixture.space(),
            std::move(memory).value(),
            fixture.memory(),
            mm::ObjectRange{0, 1},
            mm::VRange{whole.base(), mm::page_size});
        if (!ipc) {
            return false;
        }
        auto binding = Env::user(
            std::move(vspace).value(),
            std::move(cspace).value(),
            std::optional<ipc::Buffer>{
                std::move(ipc).value()});
        if (!binding || binding.value().ipc_buffer() == nullptr
            || !binding.value().ipc_buffer()->valid()) {
            return false;
        }
        auto protected_result = fixture.space().protect(
            fixture.context(),
            mm::VRange{whole.base(), mm::page_size},
            mm::Perms::of(mm::Perm::Read));
        auto unmapped = fixture.space().unmap(
            fixture.context(),
            mm::VRange{whole.base(), mm::page_size});
        // Splitting away the unborrowed tail keeps the IPC backing stable.
        auto tail = fixture.space().unmap(fixture.context(),
            mm::VRange{mm::Virt{whole.base().raw()+mm::page_size},mm::page_size});
        if (!tail || *tail != mm::VmStatus::Complete || !binding->ipc_buffer()->valid()) return false;
        if (protected_result
            || protected_result.error() != mm::VSpaceError::Busy
            || unmapped || unmapped.error() != mm::VSpaceError::Busy
            || !fixture.retire_memory()) {
            return false;
        }
        fixture.run_work();
        invalidated = !fixture.space().pending()
            && !binding.value().ipc_buffer()->valid()
            && fixture.memory().state() == mm::MemState::Retired;
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

    constexpr resource::budget limit{
        .memory = 16 * mm::page_size,
    };
    auto pending_pool = vspace_test_groups->create(*vspace_test_pmm, limit);
    if (!pending_pool) {
        return false;
    }
    auto pool = std::move(pending_pool).value().publish();
    auto pool_ref = pool.erase();
    if (!pool_ref) {
        return false;
    }
    constexpr auto fixed =
        object::pool<mm::VSpace>::slot_charge();
    auto reserved = pool->reserve(std::move(pool_ref).value(), fixed);
    if (!reserved) {
        return false;
    }
    auto pending_space = vspace_test_mm->get<mm::VSpace>().create(std::move(reserved).value(), *vspace_test_pmm, *vspace_test_kernel, *vspace_work);
    if (!pending_space) {
        return false;
    }
    auto space = std::move(pending_space).value().publish();

    const resource::budget root_baseline{
        .memory = limit.memory - fixed.memory
            - mm::page_size,
    };
    if (pool->available() != root_baseline
        || pool->sponsorship_count() != 1) {
        return false;
    }

    auto memory_ref = fixture.memory_ref();
    if (!memory_ref) {
        return false;
    }
    const mm::VRange range{
        mm::Virt{0xe0000},
        mm::page_size,
    };
    auto mapped = space->map(
        fixture.context(),
        mm::MapReq{
            range,
            mm::ObjectRange{0, 1},
            mm::Perms::of(
                mm::Perm::Read,
                mm::Perm::Write),
        },
        std::move(memory_ref).value(),
        fixture.memory(),
        fixture.memory_authority(1));
    if (!mapped || mapped.value().status != mm::VmStatus::Complete
        || pool->available().memory >= root_baseline.memory) {
        return false;
    }

    auto unmapped = space->unmap(
        fixture.context(), range);
    if (!unmapped || unmapped.value() != mm::VmStatus::Complete
        || pool->available() != root_baseline
        || pool->sponsorship_count() != 1) {
        return false;
    }

    if (!space.retire()) {
        return false;
    }
    fixture.run_work();
    if (space->state() != mm::VSpaceState::Quiescent) return false;
    space.reset();
    while (vspace_work->run()) {}


    if (pool->available() != limit || pool->sponsorship_count() != 0
        || pool->close() != object::group::phase::closed
        || !pool.retire()) {
        return false;
    }
    pool.reset();
    while (vspace_work->run()) {}


    return fixture.pmm().verify_invariants();
}

// A real initialization failure must refund a private target without a grant root.
bool test_private_creation_refunds_failed_root(const TestContext&) noexcept {
    VSpaceFixture fixture;
    if (!fixture.initialize(true, 1)) return false;
    const auto fee = object::group::allocation_charge();
    const auto slot = object::pool<mm::VSpace>::slot_charge();
    const resource::budget limit{fee.memory + slot.memory, fee.caps + slot.caps};
    auto pending = vspace_test_groups->create(fixture.pmm(), limit);
    if (!pending) return false;
    auto pool = std::move(*pending).publish();
    bool failed{};
    {
        auto self = pool.erase();
        if (!self) return false;
        auto txn = pool->begin(std::move(*self));
        auto sponsor = pool.erase();
        if (!txn || !sponsor) return false;
        auto charge = pool->reserve(std::move(*sponsor), slot);
        if (!charge) return false;
        auto space = txn->make(vspace_test_mm->get<mm::VSpace>(), std::move(*charge),
            fixture.pmm(), *vspace_test_kernel, *vspace_work);
        failed = !space && space.error() == mm::VSpaceError::ResourceExhausted;
    }
    while (vspace_work->run()) {}
    const bool refunded = pool->available() == limit && pool->sponsorship_count() == 0;
    const bool closed = pool->close() == object::group::phase::closed && pool.retire();
    pool.reset();

    return failed && refunded && closed && fixture.pmm().verify_invariants();
}

} // namespace

void register_vspace_tests(TestRegistry& registry) noexcept {
    (void)registry.add("vspace", "failed private root initialization drains and refunds", test_private_creation_refunds_failed_root);
    (void)registry.add(
        "vspace",
        "semantic map, protect and arbitrary unmap share one layout truth",
        test_semantic_map_protect_and_arbitrary_unmap);
    (void)registry.add(
        "vspace",
        "lazy fault materialization survives mapping split",
        test_lazy_fault_materialization_and_split_unmap);
    (void)registry.add("vspace", "mapping during page-in joins one backing request",
        test_mapping_during_page_in_joins_existing_request);
    (void)registry.add(
        "vspace",
        "capability revoke waits for PTE and alias retirement",
        test_capability_mapping_revokes_after_hardware_retirement);
    (void)registry.add(
        "vspace",
        "child Region and capability publish in one transaction",
        test_vm_slice_is_capability_only);
    (void)registry.add(
        "vspace",
        "Memory retirement invalidates mapping and hardware projection",
        test_memory_retire_invalidates_mapping_projection);
    (void)registry.add(
        "vspace",
        "Env blocks retirement of effective roots",
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
