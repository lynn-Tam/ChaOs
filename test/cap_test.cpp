#include <test/test.hpp>

#include <arch/address_layout.hpp>
#include <cap/cspace.hpp>
#include <cap/grant_graph.hpp>
#include <libk/manual_lifetime.hpp>
#include <libk/noncopyable.hpp>
#include <libk/utility.hpp>
#include <mm/pmm.hpp>
#include <mm/vspace_work.hpp>
#include <object/object_store.hpp>
#include <platform/memory_layout.hpp>
#include <sched/context.hpp>
#include <thread/thread.hpp>

namespace {

using kernel::cap::CapView;
using kernel::cap::CSpace;
using kernel::cap::GrantCeiling;
using kernel::cap::GrantError;
using kernel::cap::GrantGraph;
using kernel::cap::GrantRef;
using kernel::cap::Right;
using kernel::cap::Rights;

constexpr usize cap_test_page_count = 96;
alignas(kernel::mm::page_size) byte
    cap_test_ram[cap_test_page_count * kernel::mm::page_size]{};
constinit libk::ManualLifetime<kernel::mm::RegionList> cap_test_map{};
constinit libk::ManualLifetime<kernel::mm::DirectMap> cap_test_direct{};
constinit libk::ManualLifetime<kernel::mm::Pmm> cap_test_pmm{};
constinit libk::ManualLifetime<kernel::object::ObjectStore>
    cap_test_objects{};
constinit libk::ManualLifetime<kernel::mm::VSpaceExecutor> cap_test_vspace_work{};
constinit libk::ManualLifetime<GrantGraph> cap_test_graph{};
constinit libk::ManualLifetime<CSpace> cap_test_space_a{};
constinit libk::ManualLifetime<CSpace> cap_test_space_b{};
constinit libk::ManualLifetime<CSpace> cap_test_space_one{};

constexpr Rights inspect_rights = Rights::of(Right::Inspect);
constexpr Rights basic_rights = Rights::of(
    Right::Duplicate, Right::Delegate, Right::Inspect);
constexpr Rights all_context_rights = Rights::of(
    Right::Duplicate, Right::Delegate, Right::Inspect, Right::Control);

struct RevokeProbe final {
    void ready() noexcept { ++signals; }
    usize signals{};
};

class CapFixture final : private libk::noncopyable_nonmovable {
public:
    CapFixture() noexcept = default;
    ~CapFixture() noexcept { reset(); }

    [[nodiscard]] auto initialize() noexcept -> bool {
        reset();
        const auto physical = platform::memory::linked_physical(kernel::mm::VirtAddr{
            reinterpret_cast<usize>(cap_test_ram)});
        if (!physical) {
            return false;
        }
        const auto first = kernel::mm::Page::from_base(*physical);
        if (!first) {
            return false;
        }
        auto& map = cap_test_map.emplace();
        if (!map.try_emplace_back(kernel::mm::Region{
                kernel::mm::PageRange{*first, cap_test_page_count},
                kernel::mm::RegionKind::AvailableRam})) {
            reset();
            return false;
        }
        const auto direct = kernel::mm::DirectMap::initialize_in(
            cap_test_direct,
            map,
            kernel::mm::DirectMapLayout{
                .physical_base = *physical,
                .virtual_base = kernel::mm::VirtAddr{
                    reinterpret_cast<usize>(cap_test_ram)},
                .window_size = sizeof(cap_test_ram),
            });
        if (!direct
            || !kernel::mm::Pmm::initialize_in(
                cap_test_pmm,
                *cap_test_direct,
                libk::move(map))) {
            reset();
            return false;
        }
        cap_test_map.reset();
        auto& vspace_work = cap_test_vspace_work.emplace();
        [[maybe_unused]] auto& objects =
            cap_test_objects.emplace(*cap_test_pmm, vspace_work);
        [[maybe_unused]] auto& graph =
            cap_test_graph.emplace(*cap_test_pmm);
        [[maybe_unused]] auto& space_a =
            cap_test_space_a.emplace(*cap_test_pmm);
        [[maybe_unused]] auto& space_b =
            cap_test_space_b.emplace(*cap_test_pmm);
        [[maybe_unused]] auto& space_one = cap_test_space_one.emplace(
            *cap_test_pmm,
            CSpace::Quota{.slots = 1, .pages = 3});

        for (usize index = 0; index < 2; ++index) {
            auto pending = cap_test_objects->create_context(
                kernel::sched::SchedulingContext::Config{
                    .budget = kernel::time::Duration::from_ticks(index + 1),
                    .period = kernel::time::Duration::from_ticks(10),
                },
                kernel::time::Instant::from_ticks(0));
            if (!pending) {
                reset();
                return false;
            }
            targets_[index] = libk::move(pending).value().publish();
        }
        return true;
    }

    [[nodiscard]] auto root(
        usize target,
        Rights rights = all_context_rights) noexcept
        -> libk::Expected<GrantRef, GrantError> {
        if (target >= 2 || !targets_[target]) {
            return libk::unexpected(GrantError::InvalidKey);
        }
        auto reference = targets_[target].ref();
        if (!reference) {
            return libk::unexpected(GrantError::InvalidState);
        }
        return graph().create_root(
            libk::move(reference).value(),
            GrantCeiling{rights});
    }

    [[nodiscard]] auto graph() noexcept -> GrantGraph& {
        return *cap_test_graph;
    }
    [[nodiscard]] auto a() noexcept -> CSpace& { return *cap_test_space_a; }
    [[nodiscard]] auto b() noexcept -> CSpace& { return *cap_test_space_b; }
    [[nodiscard]] auto one() noexcept -> CSpace& {
        return *cap_test_space_one;
    }
    [[nodiscard]] auto target(usize index) noexcept
        -> kernel::sched::SchedulingContext& {
        return targets_[index].get();
    }
    [[nodiscard]] auto objects() noexcept
        -> kernel::object::ObjectStore& {
        return *cap_test_objects;
    }
    void drop_retired_target(usize index) noexcept {
        KASSERT(index < 2 && targets_[index]);
        targets_[index].reset();
        cap_test_objects->drain_reclaim();
    }

private:
    void reset() noexcept {
        if (cap_test_space_one) {
            cap_test_space_one->retire();
            cap_test_space_one.reset();
        }
        if (cap_test_space_b) {
            cap_test_space_b->retire();
            cap_test_space_b.reset();
        }
        if (cap_test_space_a) {
            cap_test_space_a->retire();
            cap_test_space_a.reset();
        }
        cap_test_graph.reset();
        for (auto& target : targets_) {
            if (target) {
                KASSERT(target.retire());
                target.reset();
            }
        }
        if (cap_test_objects) {
            cap_test_objects->drain_reclaim();
        }
        cap_test_objects.reset();
        cap_test_vspace_work.reset();
        cap_test_pmm.reset();
        cap_test_direct.reset();
        cap_test_map.reset();
    }

    kernel::object::ObjectStore::SchedulingContextHold targets_[2]{};
};

bool test_resolve_composes_authority_and_pins_kind(
    const TestContext&) noexcept {
    CapFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }
    auto root = fixture.root(0);
    if (!root) {
        return false;
    }
    auto inserted = fixture.a().insert(
        libk::move(root).value(), CapView{basic_rights});
    if (!inserted) {
        return false;
    }
    const auto handle = inserted.value();
    {
        auto resolved = fixture.a().resolve<
            kernel::sched::SchedulingContext>(handle, inspect_rights);
        auto wrong_kind = fixture.a().resolve<kernel::Thread>(
            handle, inspect_rights);
        auto denied = fixture.a().resolve<
            kernel::sched::SchedulingContext>(
                handle, Rights::of(Right::Control));
        if (!resolved
            || &resolved.value().object() != &fixture.target(0)
            || !resolved.value().rights().contains(basic_rights)
            || wrong_kind
            || wrong_kind.error() != kernel::cap::CSpaceError::WrongKind
            || denied
            || denied.error() != kernel::cap::CSpaceError::Denied) {
            return false;
        }
    }
    return fixture.a().close(handle)
        && fixture.graph().live_count() == 0;
}

bool test_duplicate_attenuates_without_splitting_grant(
    const TestContext&) noexcept {
    CapFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }
    auto root = fixture.root(0);
    if (!root) {
        return false;
    }
    auto source = fixture.a().insert(
        libk::move(root).value(), CapView{basic_rights});
    if (!source) {
        return false;
    }
    auto amplified = fixture.a().duplicate(
        source.value(), fixture.b(), CapView{Rights::of(Right::Control)});
    if (amplified
        || amplified.error() != kernel::cap::CSpaceError::Amplification) {
        return false;
    }
    auto copy = fixture.a().duplicate(
        source.value(), fixture.b(), CapView{inspect_rights});
    if (!copy) {
        return false;
    }
    {
        auto original = fixture.a().resolve<
            kernel::sched::SchedulingContext>(source.value(), inspect_rights);
        auto duplicate = fixture.b().resolve<
            kernel::sched::SchedulingContext>(copy.value(), inspect_rights);
        auto denied = fixture.b().resolve<
            kernel::sched::SchedulingContext>(
                copy.value(), Rights::of(Right::Delegate));
        if (!original || !duplicate
            || original.value().grant() != duplicate.value().grant()
            || denied
            || denied.error() != kernel::cap::CSpaceError::Denied) {
            return false;
        }
    }
    return fixture.b().close(copy.value())
        && fixture.a().close(source.value())
        && fixture.graph().live_count() == 0;
}

bool test_delegation_revoke_waits_for_existing_lease(
    const TestContext&) noexcept {
    CapFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }
    auto root = fixture.root(0);
    if (!root) {
        return false;
    }
    const auto root_key = root.value().key();
    auto source = fixture.a().insert(
        libk::move(root).value(), CapView{all_context_rights});
    if (!source) {
        return false;
    }
    auto child = fixture.a().delegate(
        source.value(),
        fixture.b(),
        GrantCeiling{inspect_rights},
        CapView{inspect_rights});
    if (!child) {
        return false;
    }

    RevokeProbe probe{};
    kernel::cap::GrantRevoke completion{
        kernel::sync::Completion::Notifier::bind<
            &RevokeProbe::ready>(probe)};
    kernel::cap::GrantKey child_key{};
    {
        auto active_result = fixture.b().resolve<
            kernel::sched::SchedulingContext>(child.value(), inspect_rights);
        if (!active_result) {
            return false;
        }
        auto active = libk::move(active_result).value();
        child_key = active.grant();
        if (!fixture.graph().revoke_descendants(root_key, completion)
            || completion.complete() || !completion.arm()) {
            return false;
        }
        auto rejected = fixture.b().resolve<
            kernel::sched::SchedulingContext>(child.value(), inspect_rights);
        auto source_live = fixture.a().resolve<
            kernel::sched::SchedulingContext>(source.value(), inspect_rights);
        if (rejected
            || rejected.error()
                != kernel::cap::CSpaceError::GrantUnavailable
            || !source_live) {
            return false;
        }
    }
    const auto child_state = fixture.graph().state(child_key);
    if (!completion.complete() || probe.signals != 1
        || !child_state
        || child_state.value() != kernel::cap::GrantState::Revoked) {
        return false;
    }
    return fixture.b().close(child.value())
        && fixture.a().close(source.value())
        && fixture.graph().live_count() == 0;
}

bool test_handles_are_local_and_stale_generation_stays_dead(
    const TestContext&) noexcept {
    CapFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }
    auto root_a = fixture.root(0);
    auto root_b = fixture.root(1);
    if (!root_a || !root_b) {
        return false;
    }
    auto handle_a = fixture.a().insert(
        libk::move(root_a).value(), CapView{inspect_rights});
    auto handle_b = fixture.b().insert(
        libk::move(root_b).value(), CapView{inspect_rights});
    if (!handle_a || !handle_b
        || handle_a.value().raw() != handle_b.value().raw()) {
        return false;
    }
    {
        auto resolved_a = fixture.a().resolve<
            kernel::sched::SchedulingContext>(handle_a.value(), inspect_rights);
        auto resolved_b = fixture.b().resolve<
            kernel::sched::SchedulingContext>(handle_b.value(), inspect_rights);
        if (!resolved_a || !resolved_b
            || &resolved_a.value().object() != &fixture.target(0)
            || &resolved_b.value().object() != &fixture.target(1)) {
            return false;
        }
    }
    const auto stale = handle_a.value();
    if (!fixture.a().close(stale)) {
        return false;
    }
    auto replacement_root = fixture.root(0);
    if (!replacement_root) {
        return false;
    }
    auto replacement = fixture.a().insert(
        libk::move(replacement_root).value(), CapView{inspect_rights});
    auto rejected = fixture.a().resolve<
        kernel::sched::SchedulingContext>(stale, inspect_rights);
    if (!replacement
        || replacement.value().index() != stale.index()
        || replacement.value().generation() == stale.generation()
        || rejected
        || rejected.error() != kernel::cap::CSpaceError::InvalidHandle) {
        return false;
    }
    return fixture.a().close(replacement.value())
        && fixture.b().close(handle_b.value())
        && fixture.graph().live_count() == 0;
}

bool test_move_is_transactional_across_cspaces(
    const TestContext&) noexcept {
    CapFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }
    auto source_root = fixture.root(0);
    auto blocker_root = fixture.root(1);
    if (!source_root || !blocker_root) {
        return false;
    }
    auto source = fixture.a().insert(
        libk::move(source_root).value(), CapView{inspect_rights});
    auto blocker = fixture.one().insert(
        libk::move(blocker_root).value(), CapView{inspect_rights});
    if (!source || !blocker) {
        return false;
    }
    auto rejected = fixture.a().move(source.value(), fixture.one());
    {
        auto source_live = fixture.a().resolve<
            kernel::sched::SchedulingContext>(source.value(), inspect_rights);
        if (rejected
            || rejected.error() != kernel::cap::CSpaceError::SlotQuota
            || !source_live) {
            return false;
        }
    }
    if (!fixture.one().close(blocker.value())) {
        return false;
    }
    auto moved = fixture.a().move(source.value(), fixture.one());
    if (!moved) {
        return false;
    }
    auto stale = fixture.a().resolve<kernel::sched::SchedulingContext>(
        source.value(), inspect_rights);
    {
        auto destination = fixture.one().resolve<
            kernel::sched::SchedulingContext>(moved.value(), inspect_rights);
        if (stale || !destination
            || &destination.value().object() != &fixture.target(0)) {
            return false;
        }
    }
    return fixture.one().close(moved.value())
        && fixture.graph().live_count() == 0;
}

bool test_cspace_retire_waits_for_reserved_operation(
    const TestContext&) noexcept {
    CapFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }
    {
        auto reserved = fixture.a().reserve();
        if (!reserved || fixture.a().table_pages() != 3) {
            return false;
        }
        auto reservation = libk::move(reserved).value();
        fixture.a().retire();
        auto stopped = fixture.a().reserve();
        if (stopped
            || stopped.error() != kernel::cap::CSpaceError::InvalidState
            || fixture.a().table_pages() != 3
            || fixture.a().live_slots() != 1) {
            return false;
        }
    }
    if (fixture.a().table_pages() != 0
        || fixture.a().live_slots() != 0) {
        return false;
    }

    auto pending = fixture.objects().create_cspace();
    if (!pending) {
        return false;
    }
    auto held = libk::move(pending).value().publish();
    const auto id = held.id();
    auto pin_result = fixture.objects().pin_cspace(id);
    if (!pin_result) {
        return false;
    }
    auto pinned = libk::move(pin_result).value();
    {
        auto reserved = pinned->reserve();
        if (!reserved) {
            return false;
        }
        auto reservation = libk::move(reserved).value();
        if (!held.retire()) {
            return false;
        }
        held.reset();
        if (pinned->table_pages() != 3) {
            return false;
        }
    }
    if (pinned->table_pages() != 0) {
        return false;
    }
    pinned.reset();
    fixture.objects().drain_reclaim();
    return !fixture.objects().hold_cspace(id)
        && cap_test_pmm->verify_invariants();
}

bool test_attenuated_operations_and_revoke_use_slot_authority(
    const TestContext&) noexcept {
    CapFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }
    const Rights source_rights = Rights::of(
        Right::Duplicate,
        Right::Delegate,
        Right::Inspect,
        Right::Revoke);
    auto root = fixture.root(0, source_rights);
    if (!root) {
        return false;
    }
    auto source = fixture.a().insert(
        libk::move(root).value(), CapView{source_rights});
    if (!source) {
        return false;
    }
    auto duplicate = fixture.a().duplicate(
        source.value(), fixture.b(), inspect_rights);
    auto child = fixture.a().delegate(
        source.value(), fixture.b(), inspect_rights);
    if (!duplicate || !child) {
        return false;
    }
    kernel::cap::GrantRevoke completion{};
    if (!fixture.a().revoke(source.value(), completion, false)
        || !completion.complete()) {
        return false;
    }
    auto shared = fixture.b().resolve<kernel::sched::SchedulingContext>(
        duplicate.value(), inspect_rights);
    auto revoked = fixture.b().resolve<kernel::sched::SchedulingContext>(
        child.value(), inspect_rights);
    return shared && !revoked
        && fixture.b().close(child.value())
        && fixture.b().close(duplicate.value())
        && fixture.a().close(source.value());
}

bool test_destroy_authority_uses_object_anchor_retirement(
    const TestContext&) noexcept {
    CapFixture fixture{};
    if (!fixture.initialize()) {
        return false;
    }
    const Rights rights = Rights::of(Right::Inspect, Right::Destroy);
    auto root = fixture.root(0, rights);
    if (!root) {
        return false;
    }
    auto handle = fixture.a().insert(
        libk::move(root).value(), CapView{rights});
    if (!handle || !fixture.a().destroy(handle.value())) {
        return false;
    }
    auto rejected = fixture.a().resolve<kernel::sched::SchedulingContext>(
        handle.value(), inspect_rights);
    if (rejected || !fixture.a().close(handle.value())) {
        return false;
    }
    fixture.drop_retired_target(0);
    return true;
}

} // namespace

void register_cap_tests(TestRegistry& registry) noexcept {
    (void)registry.add(
        "cap",
        "resolve composes authority and pins the typed target",
        test_resolve_composes_authority_and_pins_kind);
    (void)registry.add(
        "cap",
        "duplicate attenuates a shared grant without amplification",
        test_duplicate_attenuates_without_splitting_grant);
    (void)registry.add(
        "cap",
        "delegation revoke blocks new use and drains old leases",
        test_delegation_revoke_waits_for_existing_lease);
    (void)registry.add(
        "cap",
        "handles are CSpace-local and stale generations stay dead",
        test_handles_are_local_and_stale_generation_stays_dead);
    (void)registry.add(
        "cap",
        "move preserves source when destination transaction fails",
        test_move_is_transactional_across_cspaces);
    (void)registry.add(
        "cap",
        "CSpace retirement waits for reserved operations and pinned teardown",
        test_cspace_retire_waits_for_reserved_operation);
    (void)registry.add(
        "cap",
        "attenuated ABI operations and revoke use effective slot authority",
        test_attenuated_operations_and_revoke_use_slot_authority);
    (void)registry.add(
        "cap",
        "destroy authority enters the target ObjectAnchor retirement path",
        test_destroy_authority_uses_object_anchor_retirement);
}
