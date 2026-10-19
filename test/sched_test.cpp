#include <test/test.hpp>
#include <time.hpp>
#include <limits>

#include <libk/manual_lifetime.hpp>
#include <utility>
#include <mm/kspace.hpp>
#include <mm/pmm.hpp>
#include <object/pool.hpp>
#include <mm/mem.hpp>
#include <mm/vspace.hpp>
#include <mm/pager.hpp>
#include <boot/info.hpp>
#include <sched/sc.hpp>
#include <sched/sched.hpp>
#include <task/thread.hpp>

#include <mm/table.hpp>

namespace {

struct unregistered final {};

static_assert(object::kind<unregistered> == ObjInvalid);

using sched::Sc;

// Non-decimal frequencies exercise rounding; the maximum frequency exercises
// fractional arithmetic without overflowing intermediate products.
static_assert([] {
    constexpr time::Clock clock{32'768};
    constexpr time::Clock limit{std::numeric_limits<u64>::max()};
    const int n = 2;
    return *clock.ticks<2, time::ms>() == 66
        && clock.ticks<time::ms>(n) == clock.ticks<2, time::ms>()
        && *clock.ticks<1, time::us>() == 1
        && *clock.ticks<1, time::ns>() == 1
        && *clock.ticks<1, time::s>() == 32'768
        && *clock.ticks<0, time::ns>() == 0
        && !clock.ticks<time::ms>(-1)
        && *limit.ticks<1, time::s>() == std::numeric_limits<u64>::max()
        && *limit.ticks<1, time::ns>() == 18'446'744'074
        && !limit.ticks<2, time::s>()
        && !limit.ticks<1'000'000'001, time::ns>();
}());

constexpr usize sched_test_page_count = 320;
alignas(mm::page_size) byte
    sched_test_ram[sched_test_page_count * mm::page_size]{};
// The full boot-memory map is PMM construction workspace, so the test fixture
// owns that storage and drops it immediately after initialization instead of
// charging it to every test.
constinit libk::manual_lifetime<mm::RegionList> sched_test_memory_map{};
constinit libk::manual_lifetime<mm::Pmm> sched_test_pmm{};
constinit libk::manual_lifetime<object::store<mm::VSpace, mm::Mem, Pager>> sched_test_mm{};
constinit libk::manual_lifetime<WorkQueue> sched_work{};
constinit libk::manual_lifetime<object::store<Thread, resource::Account>> sched_test_tasks{};
constinit libk::manual_lifetime<mm::KSpace> sched_test_kernel{};

void unused_thread_entry(void*) noexcept {}

class SchedStorageGuard final {
public:
    SchedStorageGuard() noexcept { reset(); }
    ~SchedStorageGuard() noexcept { reset(); }

    [[nodiscard]] auto initialize() noexcept -> bool {
        (void)sched_work.emplace();
        const auto physical = boot_layout.phys(mm::va_t{
            reinterpret_cast<usize>(sched_test_ram)});
        if (!physical) {
            return false;
        }
        const auto first = mm::Page::from_base(*physical);
        if (!first) {
            return false;
        }
        auto& map = sched_test_memory_map.emplace();
        if (!map.try_emplace_back(mm::Region{
                mm::Pages{*first, sched_test_page_count},
                mm::Region::Kind::Ram})) {
            reset();
            return false;
        }
        if (!mm::Pmm::initialize_in(
                sched_test_pmm, std::move(map), mm::Pmm::Window{
                .pa = mm::pa_t{
                    physical->raw()},
                .va = mm::va_t{
                    reinterpret_cast<usize>(sched_test_ram)},
                .size = sizeof(sched_test_ram),
            })) {
            reset();
            return false;
        }
        sched_test_memory_map.reset();
        auto builder = mm::PageTable::create(*sched_test_pmm, mm::PageTable::Kind::Kernel);
        if (!builder) {
            reset();
            return false;
        }
        (void)sched_test_kernel.emplace(*sched_test_pmm, std::move(*builder), mm::va_t{boot_layout.va}.raw());
        [[maybe_unused]] auto& memory =
            sched_test_mm.emplace(*sched_test_pmm, *sched_work);
        (void)sched_test_tasks.emplace(*sched_test_pmm, *sched_work);
        return true;
    }

private:
    static void reset() noexcept {
        sched_test_mm.reset();
        sched_test_tasks.reset();
        sched_test_kernel.reset();
        sched_work.reset();
        sched_test_pmm.reset();
        sched_test_memory_map.reset();
    }
};

bool test_refill_conserves_and_delays_budget(const TestContext&) noexcept {
    sched::Budget aggregate{10, 100};
    auto first = aggregate.reserve(0, 7), second = aggregate.reserve(0, 7);
    const auto reserved = aggregate.info(0);
    if (first.amount != 7 || second.amount != 3 || reserved.available || reserved.escrow != 10)
        return false;
    aggregate.settle(first, 3, 8);
    aggregate.settle(second, 5, 5);
    const auto used = aggregate.info(5);
    if (used.balance + used.refill + used.escrow != 10 || used.debt != 3
        || used.available || aggregate.info(103).available != 5 || aggregate.info(132).available != 5 || aggregate.info(133).available != 10)
        return false;
    sched::Budget repeated{10, 100};
    for (u64 now : {0U, 200U, 400U}) {
        const auto loan = repeated.reserve(now, 10);
        if (loan.amount != 10) return false;
        repeated.settle(loan, now, 20);
        if (repeated.info(now + 199).available || repeated.info(now + 200).available != 10)
            return false;
    }
    Sc queue{{.budget=10, .period=100,
              .refill_capacity=4}, 0};
    if (queue.available(0) != 10) {
        return false;
    }
    if (queue.charge(3, 4) != 0
        || queue.available(3) != 6) {
        return false;
    }
    if (queue.charge(5, 6) != 0
        || queue.available(102) != 0
        || queue.available(103) != 4
        || queue.available(105) != 10) {
        return false;
    }
    const u64 overrun = queue.charge(
        105, 13);
    return overrun == 3
        && queue.available(234) == 0
        && queue.available(235) == 10;
}

bool test_bounded_refill_merge_never_advances_budget(
    const TestContext&) noexcept {
    Sc queue{{.budget=8, .period=40,
              .refill_capacity=1}, 0};
    (void)queue.charge(1, 2);
    (void)queue.charge(2, 2);
    (void)queue.charge(3, 2);

    return queue.available(40) == 2
        && queue.available(41) == 2
        && queue.available(42) == 2
        && queue.available(43) == 8;
}

bool test_scheduling_context_config_boundaries(
    const TestContext&) noexcept {
    using Config = sched::Sc::Config;
    const auto valid = [](u64 budget, u64 period, usize capacity) noexcept {
        return sched::Sc::valid_config(Config{
            .budget = budget,
            .period = period,
            .refill_capacity = capacity,
        });
    };
    return valid(1, 1, 1)
        && valid(4, 8, sched::Sc::max_refills)
        && !valid(0, 1, 1)
        && !valid(1, 0, 1)
        && !valid(2, 1, 1)
        && !valid(1, 1, 0)
        && !valid(
            1, 1, sched::Sc::max_refills + 1)
        && valid(0, 0, 1)
        && !Sc::valid_config({.weight = 0})
        && !Sc::valid_config({.weight = 65536});
}

bool test_object_store_unpublished_construction_rolls_back(
    const TestContext&) noexcept {
    SchedStorageGuard storage{};
    if (!storage.initialize()) {
        return false;
    }
    // KSpace owns mapped guarded stack slots as a reusable pool. Warm
    // one slot so this test measures Thread construction rollback, not pool
    // growth.
    {
        auto warm = mm::Stack::create(*sched_test_kernel);
        if (!warm) {
            return false;
        }
    }
    const usize free_before = sched_test_pmm->free_page_count();
    {
        auto stack = mm::Stack::create(*sched_test_kernel);
        if (!stack) {
            return false;
        }
        auto pending = sched_test_tasks->get<Thread>().create(
            std::move(stack).value(),
            Env::kernel(*sched_test_kernel),
            Thread::KernelStart{unused_thread_entry, nullptr});
        if (!pending) {
            return false;
        }
        // Pending owns rollback until publication transfers the initial reference.
    }
    while (sched_work->run()) {}
    return sched_test_pmm->free_page_count() == free_before
        && sched_test_pmm->verify_invariants();
}

bool test_resource_pool_refunds_after_object_reclaim(
    const TestContext&) noexcept {
    SchedStorageGuard storage{};
    if (!storage.initialize()) {
        return false;
    }

    constexpr resource::budget limit{
        .memory = 3 * mm::page_size,
        .caps = 4,
    };
    constexpr resource::budget charge{
        .memory = mm::page_size,
        .caps = 1,
    };
    auto pending_pool = sched_test_tasks->get<resource::Account>().create(limit);
    if (!pending_pool) {
        return false;
    }
    auto pool = std::move(pending_pool).value().publish();

    // An abandoned pre-commit reservation has created no resource and must
    // restore the ledger immediately.
    {
        auto self = object::ref<>{pool};
        if (!self) {
            return false;
        }
        auto reserved = pool->reserve(std::move(self), charge);
        if (!reserved
            || pool->available()
                != resource::budget{
                    .memory = limit.memory - charge.memory,
                    .caps = limit.caps - charge.caps}) {
            return false;
        }
    }
    if (pool->available() != limit || pool->info(0).allocations != 0) {
        return false;
    }

    auto self = object::ref<>{pool};
    if (!self) {
        return false;
    }
    auto reserved = pool->reserve(std::move(self), charge);
    if (!reserved) {
        return false;
    }
    auto pending_memory = sched_test_mm->get<mm::Mem>().create(std::move(reserved).value(), *sched_test_pmm, 2 * mm::page_size, mm::AnonCfg{});
    if (!pending_memory) {
        return false;
    }
    auto memory = std::move(pending_memory).value().publish();
    const resource::budget after_backing{
        .memory = limit.memory - charge.memory,
        .caps = limit.caps - charge.caps,
    };
    if (pool->info(0).allocations != 1
        || pool->available() != after_backing) {
        return false;
    }
    // Storage and residents carry capacity charges; only the Mem slot is registered.
    auto first_result = memory.get().materialize(0);
    if (!first_result
        || pool->available().memory != 0
        || pool->info(0).allocations != 1) {
        return false;
    }
    auto first_page = std::move(first_result).value();
    auto exhausted = memory.get().materialize(1);
    if (exhausted
        || exhausted.error() != mm::MemErr::ResourceExhausted
        || pool->available().memory != 0
        || pool->info(0).allocations != 1) {
        return false;
    }
    first_page.reset();
    if (!memory.destroy()) {
        return false;
    }
    memory.reset();

    // Retire only requests cleanup. Capacity is dishonest if it returns
    // before pool has made the slot and payload reusable.
    if (pool->info(0).allocations != 1 || pool->available() == limit) {
        return false;
    }
    while (sched_work->run()) {}
    if (pool->info(0).allocations != 0
        || pool->available() != limit
        || pool->close() != resource::Account::Phase::Drained
        || !pool.destroy()) {
        return false;
    }
    pool.reset();
    while (sched_work->run()) {}
    return sched_test_pmm->verify_invariants();
}

bool test_resource_pool_child_returns_transferred_budget(
    const TestContext&) noexcept {
    SchedStorageGuard storage{};
    if (!storage.initialize()) {
        return false;
    }

    constexpr resource::budget parent_limit{
        .memory = 16 * mm::page_size,
        .caps = 16,
    };
    constexpr resource::budget child_limit{
        .memory = 4 * mm::page_size,
        .caps = 4,
    };
    // The child owns its delegated budget. Its pool capacity is a
    // separate parent-funded cost and returns with the child object.
    constexpr resource::budget transfer{
        .memory = mm::page_size,
    };
    auto pending_parent = sched_test_tasks->get<resource::Account>().create(parent_limit);
    if (!pending_parent) {
        return false;
    }
    auto parent = std::move(pending_parent).value().publish();
    auto parent_ref = object::ref<>{parent};
    if (!parent_ref) {
        return false;
    }
    auto child_charge = parent->reserve(
        std::move(parent_ref), transfer);
    if (!child_charge) {
        return false;
    }
    auto delegation = parent->reserve(parent, child_limit);
    if (!delegation) return false;
    auto pending_child = sched_test_tasks->get<resource::Account>().create(std::move(child_charge).value(), child_limit,
        std::move(*delegation).commit());
    if (!pending_child) {
        return false;
    }
    auto child = std::move(pending_child).value().publish();

    constexpr resource::budget object_charge{
        .memory = mm::page_size,
        .caps = 1,
    };
    auto child_ref = object::ref<>{child};
    if (!child_ref) {
        return false;
    }
    auto reserved = child->reserve(
        std::move(child_ref), object_charge);
    if (!reserved) {
        return false;
    }
    auto pending_memory = sched_test_mm->get<mm::Mem>().create(std::move(reserved).value(), *sched_test_pmm, mm::page_size, mm::AnonCfg{});
    if (!pending_memory) {
        return false;
    }
    auto memory = std::move(pending_memory).value().publish();
    if (!child.destroy()) return false;
    while (sched_work->run()) {}
    if (child->state() != resource::Account::Phase::Closing
        || child->info(0).allocations != 1
        || parent->info(0).allocations != 1) {
        return false;
    }

    if (!memory.destroy()) {
        return false;
    }
    memory.reset();
    while (sched_work->run()) {}
    if (child->state() != resource::Account::Phase::Drained) {
        return false;
    }
    child.reset();
    while (sched_work->run()) {}
    if (parent->available() != parent_limit
        || parent->info(0).allocations != 0
        || parent->close() != resource::Account::Phase::Drained
        || !parent.destroy()) {
        return false;
    }
    parent.reset();
    while (sched_work->run()) {}
    return sched_test_pmm->verify_invariants();
}

bool test_group_close_waits_for_reservation(const TestContext&) noexcept {
    SchedStorageGuard storage{};
    if (!storage.initialize()) return false;
    constexpr resource::budget limit{.memory = 2 * mm::page_size, .caps = 2};
    constexpr resource::budget charge{.memory = mm::page_size, .caps = 1};
    auto pending = sched_test_tasks->get<resource::Account>().create(limit);
    if (!pending) return false;
    auto group = pending->publish();
    auto payer = object::ref<>{group};
    if (!payer) return false;
    auto fee = group->reserve(std::move(payer), charge);
    if (!fee || group->close() != resource::Account::Phase::Closing) return false;
    // An accepted reservation may finish; closing rejects new admission.
    payer = object::ref<>{group};
    if (!payer) return false;
    auto rejected = group->reserve(std::move(payer), charge);
    if (rejected || rejected.error() != resource::errc::closed) return false;
    fee->reset();
    if (group->available() != limit || group->state() != resource::Account::Phase::Drained
        || !group.destroy()) return false;
    group.reset();
    while (sched_work->run()) {}
    return sched_test_pmm->verify_invariants();
}

bool test_kernel_stack_uses_guarded_virtual_slot(
    const TestContext&) noexcept {
    SchedStorageGuard storage{};
    if (!storage.initialize()) {
        return false;
    }

    usize first_base{};
    {
        auto created = mm::Stack::create(*sched_test_kernel);
        if (!created) {
            return false;
        }
        auto stack = std::move(created).value();
        first_base = stack.base();

        const auto lower = mm::VPage::from_base(
            mm::va_t{stack.lower_guard()});
        const auto first = mm::VPage::from_base(
            mm::va_t{stack.base()});
        const auto last = mm::VPage::from_base(
            mm::va_t{stack.top() - mm::page_size});
        const auto upper = mm::VPage::from_base(
            mm::va_t{stack.upper_guard()});
        if (!lower || !first || !last || !upper) {
            return false;
        }
        const auto lower_entry = sched_test_kernel->pages().query(*lower);
        const auto first_entry = sched_test_kernel->pages().query(*first);
        const auto last_entry = sched_test_kernel->pages().query(*last);
        const auto upper_entry = sched_test_kernel->pages().query(*upper);
        if (lower_entry
            || lower_entry.error() != mm::PtErr::Missing
            || !first_entry
            || !last_entry
            || upper_entry
            || upper_entry.error() != mm::PtErr::Missing
            || stack.size() != mm::Stack::StackBytes) {
            return false;
        }
    }

    auto reused = mm::Stack::create(*sched_test_kernel);
    return reused && reused.value().base() == first_base;
}

} // namespace

void register_sched_tests(TestRegistry& registry) noexcept {
    (void)registry.add(
        "sched",
        "refill ledger conserves budget and reports overrun",
        test_refill_conserves_and_delays_budget);
    (void)registry.add(
        "sched",
        "bounded refill merge delays but never advances budget",
        test_bounded_refill_merge_never_advances_budget);
    (void)registry.add(
        "sched",
        "SC configuration validates optional caps and weight",
        test_scheduling_context_config_boundaries);
    (void)registry.add(
        "sched",
        "Object pool unpublished construction rolls back slab and payload",
        test_object_store_unpublished_construction_rolls_back);
    (void)registry.add(
        "sched",
        "ResourcePool refunds only after sponsored object reclaim",
        test_resource_pool_refunds_after_object_reclaim);
    (void)registry.add(
        "sched",
        "child ResourcePool returns its delegated budget after reclaim",
        test_resource_pool_child_returns_transferred_budget);
    (void)registry.add(
        "sched",
        "group close waits for accepted reservations",
        test_group_close_waits_for_reservation);
    (void)registry.add(
        "sched",
        "kernel stacks use guarded reusable virtual slots",
        test_kernel_stack_uses_guarded_virtual_slot);
}
