#include <test/test.hpp>
#include <limits>

#include <libk/manual_lifetime.hpp>
#include <utility>
#include <mm/kspace.hpp>
#include <mm/pmm.hpp>
#include <object/group.hpp>
#include <object/pool.hpp>
#include <mm/mem.hpp>
#include <mm/vspace.hpp>
#include <mm/pager.hpp>
#include <boot/info.hpp>
#include <sched/sc.hpp>
#include <sched/domain.hpp>
#include <sched/sched.hpp>
#include <task/thread.hpp>

#include <mm/table.hpp>

namespace {

struct unregistered final {};

static_assert(object::kind<unregistered> == object::ObjectKind::Invalid);

using sched::Sc;
using time::Duration;
using time::Instant;

constexpr usize sched_test_page_count = 320;
alignas(mm::page_size) byte
    sched_test_ram[sched_test_page_count * mm::page_size]{};
// The full boot-memory map is PMM construction workspace, so the test fixture
// owns that storage and drops it immediately after initialization instead of
// charging it to every test.
constinit libk::ManualLifetime<mm::RegionList> sched_test_memory_map{};
constinit libk::ManualLifetime<mm::Pmm> sched_test_pmm{};
constinit libk::ManualLifetime<object::store<mm::VSpace, mm::Mem, Pager>> sched_test_mm{};
constinit libk::ManualLifetime<WorkQueue> sched_work{};
constinit libk::ManualLifetime<object::store<Thread, object::group>> sched_test_tasks{};
constinit libk::ManualLifetime<object::store<sched::Domain, sched::Sc>> sched_test_sched{};
constinit libk::ManualLifetime<mm::KSpace> sched_test_kernel{};

void unused_thread_entry(void*) noexcept {}

class SchedStorageGuard final {
public:
    SchedStorageGuard() noexcept { reset(); }
    ~SchedStorageGuard() noexcept { reset(); }

    [[nodiscard]] auto initialize() noexcept -> bool {
        (void)sched_work.emplace();
        const auto physical = boot_layout.phys(mm::Virt{
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
                .pa = mm::Phys{
                    physical->raw()},
                .va = mm::Virt{
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
        (void)sched_test_kernel.emplace(*sched_test_pmm, std::move(*builder), mm::Virt{boot_layout.va}.raw());
        [[maybe_unused]] auto& memory =
            sched_test_mm.emplace(*sched_test_pmm, *sched_work);
        (void)sched_test_tasks.emplace(*sched_test_pmm, *sched_work);
        (void)sched_test_sched.emplace(*sched_test_pmm, *sched_work);
        return true;
    }

private:
    static void reset() noexcept {
        sched_test_mm.reset();
        sched_test_sched.reset();
        sched_test_tasks.reset();
        sched_test_kernel.reset();
        sched_work.reset();
        sched_test_pmm.reset();
        sched_test_memory_map.reset();
    }
};

bool test_refill_conserves_and_delays_budget(const TestContext&) noexcept {
    Sc queue{{.budget=Duration::from_ticks(10), .period=Duration::from_ticks(100),
              .refill_capacity=4}, Instant::from_ticks(0)};
    if (queue.available(Instant::from_ticks(0)).ticks() != 10) {
        return false;
    }
    if (!queue.charge(
            Instant::from_ticks(3), Duration::from_ticks(4)).empty()
        || queue.available(Instant::from_ticks(3)).ticks() != 6) {
        return false;
    }
    if (!queue.charge(
            Instant::from_ticks(5), Duration::from_ticks(6)).empty()
        || !queue.available(Instant::from_ticks(102)).empty()
        || queue.available(Instant::from_ticks(103)).ticks() != 4
        || queue.available(Instant::from_ticks(105)).ticks() != 10) {
        return false;
    }
    const Duration overrun = queue.charge(
        Instant::from_ticks(105), Duration::from_ticks(13));
    return overrun.ticks() == 3
        && queue.available(Instant::from_ticks(204)).empty()
        && queue.available(Instant::from_ticks(205)).ticks() == 10;
}

bool test_bounded_refill_merge_never_advances_budget(
    const TestContext&) noexcept {
    Sc queue{{.budget=Duration::from_ticks(8), .period=Duration::from_ticks(40),
              .refill_capacity=2}, Instant::from_ticks(0)};
    (void)queue.charge(Instant::from_ticks(1), Duration::from_ticks(2));
    (void)queue.charge(Instant::from_ticks(2), Duration::from_ticks(2));
    (void)queue.charge(Instant::from_ticks(3), Duration::from_ticks(2));

    return queue.available(Instant::from_ticks(40)).ticks() == 2
        && queue.available(Instant::from_ticks(41)).ticks() == 2
        && queue.available(Instant::from_ticks(42)).ticks() == 2
        && queue.available(Instant::from_ticks(43)).ticks() == 8;
}

bool test_refill_state_space_preserves_sliding_window(
    const TestContext&) noexcept {
    constexpr usize horizon = 64;
    for (u64 budget = 1; budget <= 4; ++budget) {
        for (u64 period = budget; period <= 8; ++period) {
            for (usize capacity = 1; capacity <= 4; ++capacity) {
                Sc queue{{.budget=Duration::from_ticks(budget),
                          .period=Duration::from_ticks(period),
                          .refill_capacity=capacity}, Instant::from_ticks(0)};
                u64 granted[horizon]{};

                for (usize tick = 0; tick < horizon; ++tick) {
                    const Instant now = Instant::from_ticks(tick);
                    const u64 available = queue.available(now).ticks();
                    const u64 demand =
                        (tick * 5 + budget * 3 + period + capacity) % 6;
                    const Duration overrun = queue.charge(
                        now, Duration::from_ticks(demand));
                    if (overrun.ticks() > demand
                        || demand - overrun.ticks()
                            != (demand < available ? demand : available)
                        || queue.available(now).ticks() > budget) {
                        return false;
                    }
                    granted[tick] = demand - overrun.ticks();

                    const usize first = tick + 1 > period
                        ? tick + 1 - period
                        : 0;
                    u64 window{};
                    for (usize index = first; index <= tick; ++index) {
                        window += granted[index];
                    }
                    if (window > budget) {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

bool test_scheduling_context_config_boundaries(
    const TestContext&) noexcept {
    using Config = sched::Sc::Config;
    const auto valid = [](u64 budget, u64 period, usize capacity) noexcept {
        return sched::Sc::valid_config(Config{
            .budget = Duration::from_ticks(budget),
            .period = Duration::from_ticks(period),
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
        && sched::Urgency::make(
            sched::Urgency::level_count - 1)
        && !sched::Urgency::make(
            sched::Urgency::level_count)
        && !sched::Urgency::make(256);
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
        // Dropping Pending is the only rollback authority. No ObjectId exists
        // until publish(), so the half-constructed object is unaddressable.
    }
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
    auto pending_pool = sched_test_tasks->get<object::group>().create(*sched_test_pmm, limit);
    if (!pending_pool) {
        return false;
    }
    auto pool = std::move(pending_pool).value().publish();

    // An abandoned pre-commit reservation has created no resource and must
    // restore the ledger immediately.
    {
        auto self = pool.erase();
        if (!self) {
            return false;
        }
        auto reserved = pool->reserve(std::move(self).value(), charge);
        if (!reserved
            || pool->available()
                != resource::budget{
                    .memory = limit.memory - charge.memory,
                    .caps = limit.caps - charge.caps}) {
            return false;
        }
    }
    if (pool->available() != limit || pool->sponsorship_count() != 0) {
        return false;
    }

    auto self = pool.erase();
    if (!self) {
        return false;
    }
    auto reserved = pool->reserve(std::move(self).value(), charge);
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
    if (pool->sponsorship_count() != 1
        || pool->available() != after_backing
        || pool->can_retire()) {
        return false;
    }
    // Storage and residents carry capacity charges; only the Mem slot is registered.
    auto first_result = memory.get().materialize(0);
    if (!first_result
        || pool->available().memory != 0
        || pool->sponsorship_count() != 1) {
        return false;
    }
    auto first_page = std::move(first_result).value();
    auto exhausted = memory.get().materialize(1);
    if (exhausted
        || exhausted.error() != mm::MemErr::ResourceExhausted
        || pool->available().memory != 0
        || pool->sponsorship_count() != 1) {
        return false;
    }
    first_page.reset();
    if (!memory.retire()) {
        return false;
    }
    memory.reset();

    // Retire only requests cleanup. Capacity is dishonest if it returns
    // before pool has made the slot and payload reusable.
    if (pool->sponsorship_count() != 1 || pool->available() == limit) {
        return false;
    }
    while (sched_work->run()) {}
    if (pool->sponsorship_count() != 0
        || pool->available() != limit
        || pool->close() != object::group::phase::closed
        || !pool->can_retire()
        || !pool.retire()) {
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
        .memory = child_limit.memory + mm::page_size,
        .caps = child_limit.caps,
    };
    auto pending_parent = sched_test_tasks->get<object::group>().create(*sched_test_pmm, parent_limit);
    if (!pending_parent) {
        return false;
    }
    auto parent = std::move(pending_parent).value().publish();
    auto parent_ref = parent.erase();
    if (!parent_ref) {
        return false;
    }
    auto child_charge = parent->reserve(
        std::move(parent_ref).value(), transfer);
    if (!child_charge) {
        return false;
    }
    auto pending_child = sched_test_tasks->get<object::group>().create(std::move(child_charge).value(), *sched_test_pmm, child_limit);
    if (!pending_child) {
        return false;
    }
    auto child = std::move(pending_child).value().publish();

    constexpr resource::budget object_charge{
        .memory = mm::page_size,
        .caps = 1,
    };
    auto child_ref = child.erase();
    if (!child_ref) {
        return false;
    }
    auto reserved = child->reserve(
        std::move(child_ref).value(), object_charge);
    if (!reserved) {
        return false;
    }
    auto pending_memory = sched_test_mm->get<mm::Mem>().create(std::move(reserved).value(), *sched_test_pmm, mm::page_size, mm::AnonCfg{});
    if (!pending_memory) {
        return false;
    }
    auto memory = std::move(pending_memory).value().publish();
    if (child.retire()
        || child->sponsorship_count() != 1
        || parent->sponsorship_count() != 1) {
        return false;
    }

    if (!memory.retire()) {
        return false;
    }
    memory.reset();
    while (sched_work->run()) {}
    if (child->close() != object::group::phase::closed
        || !child->can_retire() || !child.retire()) {
        return false;
    }
    child.reset();
    while (sched_work->run()) {}
    if (parent->available() != parent_limit
        || parent->sponsorship_count() != 0
        || parent->close() != object::group::phase::closed
        || !parent.retire()) {
        return false;
    }
    parent.reset();
    while (sched_work->run()) {}
    return sched_test_pmm->verify_invariants();
}

bool test_resource_pool_close_waits_for_open_transactions(
    const TestContext&) noexcept {
    SchedStorageGuard storage{};
    if (!storage.initialize()) {
        return false;
    }

    constexpr resource::budget limit{
        .memory = 2 * mm::page_size,
        .caps = 2,
    };
    constexpr resource::budget charge{
        .memory = mm::page_size,
        .caps = 1,
    };

    auto pending_txn_pool = sched_test_tasks->get<object::group>().create(*sched_test_pmm, limit);
    if (!pending_txn_pool) {
        return false;
    }
    auto txn_pool = std::move(pending_txn_pool).value().publish();
    auto txn_ref = txn_pool.erase();
    if (!txn_ref) {
        return false;
    }
    auto txn = txn_pool->begin(std::move(txn_ref).value());
    if (!txn
        || txn_pool->close() != object::group::phase::closing) {
        return false;
    }

    // Closing is a linearization boundary: existing construction may finish,
    // but no new construction epoch or budget reservation may enter.
    auto rejected_txn_ref = txn_pool.erase();
    auto rejected_reservation_ref = txn_pool.erase();
    if (!rejected_txn_ref || !rejected_reservation_ref) {
        return false;
    }
    auto rejected_txn = txn_pool->begin(
        std::move(rejected_txn_ref).value());
    auto rejected_reservation = txn_pool->reserve(
        std::move(rejected_reservation_ref).value(), charge);
    if (rejected_txn
        || rejected_txn.error() != resource::errc::closed
        || rejected_reservation
        || rejected_reservation.error()
            != resource::errc::closed) {
        return false;
    }
    std::move(txn).value().reset();
    if (txn_pool->state() != object::group::phase::closed
        || !txn_pool.retire()) {
        return false;
    }
    txn_pool.reset();
    while (sched_work->run()) {}

    auto pending_reservation_pool = sched_test_tasks->get<object::group>().create(*sched_test_pmm, limit);
    if (!pending_reservation_pool) {
        return false;
    }
    auto reservation_pool =
        std::move(pending_reservation_pool).value().publish();
    auto reservation_ref = reservation_pool.erase();
    if (!reservation_ref) {
        return false;
    }
    auto reservation = reservation_pool->reserve(
        std::move(reservation_ref).value(), charge);
    if (!reservation
        || reservation_pool->close() != object::group::phase::closing) {
        return false;
    }
    std::move(reservation).value().reset();
    if (reservation_pool->available() != limit
        || reservation_pool->state() != object::group::phase::closed
        || !reservation_pool.retire()) {
        return false;
    }
    reservation_pool.reset();
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
            mm::Virt{stack.lower_guard()});
        const auto first = mm::VPage::from_base(
            mm::Virt{stack.base()});
        const auto last = mm::VPage::from_base(
            mm::Virt{stack.top() - mm::page_size});
        const auto upper = mm::VPage::from_base(
            mm::Virt{stack.upper_guard()});
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

bool test_domain_admission_is_conservative_and_transactional(
    const TestContext&) noexcept {
    SchedStorageGuard storage{};
    if (!storage.initialize()) {
        return false;
    }
    auto pending_domain = sched_test_sched->get<sched::Domain>().create(
        usize{1},
        sched::Domain::share_scale,
        100'000U);
    if (!pending_domain) {
        return false;
    }
    auto domain = std::move(pending_domain).value().publish();

    object::ref<sched::Sc> contexts[3]{};
    constexpr u64 budgets[3]{1, 1, 1};
    constexpr u64 periods[3]{3, 3, 2};
    for (usize index = 0; index < 3; ++index) {
        auto pending = sched_test_sched->get<sched::Sc>().create(
            sched::Sc::Config{
                .budget = Duration::from_ticks(budgets[index]),
                .period = Duration::from_ticks(periods[index]),
            },
            Instant::from_ticks(0));
        if (!pending) {
            return false;
        }
        contexts[index] = std::move(pending).value().publish();
    }

    const bool first = static_cast<bool>(
        domain->admit(contexts[0].get(), CpuId{0}));
    const bool second = static_cast<bool>(
        domain->admit(contexts[1].get(), CpuId{0}));
    const auto rejected = domain->admit(contexts[2].get(), CpuId{0});
    const bool rejected_cleanly = !rejected
        && rejected.error()
            == sched::Domain::Error::Quota
        && !contexts[2]->admitted();
    const bool released = static_cast<bool>(
        domain->unadmit(contexts[0].get()));
    const bool admitted_after_release = static_cast<bool>(
        domain->admit(contexts[2].get(), CpuId{0}));
    const auto wrong_cpu = domain->admit(contexts[0].get(), CpuId{1});

    const bool result = first && second && rejected_cleanly && released
        && admitted_after_release && !wrong_cpu
        && wrong_cpu.error()
            == sched::Domain::Error::InvalidCpu;

    for (usize index = 1; index < 3; ++index) {
        if (!domain->unadmit(contexts[index].get())) {
            return false;
        }
    }
    // Scaling must not reject a valid ratio merely because its operands are large.
    const auto max = std::numeric_limits<u64>::max();
    Sc wide{{.budget=Duration::from_ticks(max / 2), .period=Duration::from_ticks(max)},
            Instant::from_ticks(0)};
    const bool wide_valid = domain->admit(wide, CpuId{0}) && domain->unadmit(wide);
    for (auto& context : contexts) {
        if (!context.retire()) {
            return false;
        }
        context.reset();
    }
    if (!domain.retire()) {
        return false;
    }
    domain.reset();
    while (sched_work->run()) {}
    return result && wide_valid && sched_test_pmm->verify_invariants();
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
        "refill model preserves every sampled sliding window",
        test_refill_state_space_preserves_sliding_window);
    (void)registry.add(
        "sched",
        "SC configuration rejects invalid time and urgency bounds",
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
        "ResourcePool close waits for construction and budget transactions",
        test_resource_pool_close_waits_for_open_transactions);
    (void)registry.add(
        "sched",
        "kernel stacks use guarded reusable virtual slots",
        test_kernel_stack_uses_guarded_virtual_slot);
    (void)registry.add(
        "sched",
        "domain admission rounds conservatively and rolls back failure",
        test_domain_admission_is_conservative_and_transactional);
}
