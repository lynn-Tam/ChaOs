#include <test/test.hpp>

#include <arch/address_layout.hpp>
#include <libk/manual_lifetime.hpp>
#include <libk/utility.hpp>
#include <mm/kernel_stack.hpp>
#include <mm/kernel_vspace.hpp>
#include <mm/pmm.hpp>
#include <object/object_store.hpp>
#include <platform/memory_layout.hpp>
#include <mm/vspace_work.hpp>
#include <sched/context.hpp>
#include <sched/domain.hpp>
#include <sched/ready_queue.hpp>
#include <sched/refill_queue.hpp>
#include <thread/thread.hpp>

#include "arch/riscv64/mmu/sv39_builder.hpp"

namespace {

struct MissingObjectTraits final {};

static_assert(!kernel::object::StorableObject<MissingObjectTraits>);
static_assert(kernel::object::StorableObject<kernel::Thread>);
static_assert(kernel::object::StorableObject<
    kernel::sched::SchedulingContext>);
static_assert(kernel::object::StorableObject<
    kernel::sched::SchedulingDomain>);

using kernel::sched::RefillQueue;
using kernel::time::Duration;
using kernel::time::Instant;

constexpr usize sched_test_page_count = 40;
alignas(kernel::mm::page_size) byte
    sched_test_ram[sched_test_page_count * kernel::mm::page_size]{};
// Builtin tests still execute on the boot stack. The full boot-memory map is
// PMM construction workspace, so the test fixture owns that storage and drops
// it immediately after initialization instead of charging it to every test.
constinit libk::ManualLifetime<kernel::mm::RegionList> sched_test_memory_map{};
constinit libk::ManualLifetime<kernel::mm::Pmm> sched_test_pmm{};
constinit libk::ManualLifetime<kernel::mm::DirectMap> sched_test_direct{};
constinit libk::ManualLifetime<kernel::object::ObjectStore>
    sched_test_objects{};
constinit libk::ManualLifetime<kernel::mm::VSpaceExecutor>
    sched_test_vspace_work{};
constinit libk::ManualLifetime<kernel::mm::KernelVSpace> sched_test_kernel{};

void unused_thread_entry(void*) noexcept {}

class SchedStorageGuard final {
public:
    SchedStorageGuard() noexcept { reset(); }
    ~SchedStorageGuard() noexcept { reset(); }

    [[nodiscard]] auto initialize() noexcept -> bool {
        const auto physical = platform::memory::linked_physical(kernel::mm::VirtAddr{
            reinterpret_cast<usize>(sched_test_ram)});
        if (!physical) {
            return false;
        }
        const auto first = kernel::mm::Page::from_base(*physical);
        if (!first) {
            return false;
        }
        auto& map = sched_test_memory_map.emplace();
        if (!map.try_emplace_back(kernel::mm::Region{
                kernel::mm::PageRange{*first, sched_test_page_count},
                kernel::mm::RegionKind::AvailableRam})) {
            reset();
            return false;
        }
        const auto direct = kernel::mm::DirectMap::initialize_in(
            sched_test_direct,
            map,
            kernel::mm::DirectMapLayout{
                .physical_base = kernel::mm::PhysAddr{
                    physical->raw()},
                .virtual_base = kernel::mm::VirtAddr{
                    reinterpret_cast<usize>(sched_test_ram)},
                .window_size = sizeof(sched_test_ram),
            });
        if (!direct) {
            reset();
            return false;
        }
        if (!kernel::mm::Pmm::initialize_in(
                sched_test_pmm, *sched_test_direct, libk::move(map))) {
            reset();
            return false;
        }
        sched_test_memory_map.reset();
        auto builder = arch::riscv64::Sv39Builder::create(*sched_test_pmm);
        if (!builder) {
            reset();
            return false;
        }
        arch::KernelRoot root = libk::move(builder).value().finalize();
        if (!kernel::mm::KernelVSpace::adopt_in(
                sched_test_kernel, *sched_test_pmm, libk::move(root))) {
            reset();
            return false;
        }
        auto& vspace_work = sched_test_vspace_work.emplace();
        [[maybe_unused]] auto& objects =
            sched_test_objects.emplace(*sched_test_pmm, vspace_work);
        return true;
    }

private:
    static void reset() noexcept {
        sched_test_objects.reset();
        sched_test_vspace_work.reset();
        sched_test_kernel.reset();
        sched_test_pmm.reset();
        sched_test_direct.reset();
        sched_test_memory_map.reset();
    }
};

bool test_refill_conserves_and_delays_budget(const TestContext&) noexcept {
    RefillQueue queue{
        Duration::from_ticks(10), Duration::from_ticks(100), 4,
        Instant::from_ticks(0)};
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
    RefillQueue queue{
        Duration::from_ticks(8), Duration::from_ticks(40), 2,
        Instant::from_ticks(0)};
    (void)queue.charge(Instant::from_ticks(1), Duration::from_ticks(2));
    (void)queue.charge(Instant::from_ticks(2), Duration::from_ticks(2));
    (void)queue.charge(Instant::from_ticks(3), Duration::from_ticks(2));

    return queue.size() == 2
        && queue.available(Instant::from_ticks(40)).ticks() == 2
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
                RefillQueue queue{
                    Duration::from_ticks(budget),
                    Duration::from_ticks(period),
                    capacity,
                    Instant::from_ticks(0)};
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
                        || queue.size() > capacity
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
    using Config = kernel::sched::SchedulingContext::Config;
    const auto valid = [](u64 budget, u64 period, usize capacity) noexcept {
        return kernel::sched::SchedulingContext::valid_config(Config{
            .budget = Duration::from_ticks(budget),
            .period = Duration::from_ticks(period),
            .refill_capacity = capacity,
        });
    };
    return valid(1, 1, 1)
        && valid(4, 8, kernel::sched::SchedulingContext::max_refills)
        && !valid(0, 1, 1)
        && !valid(1, 0, 1)
        && !valid(2, 1, 1)
        && !valid(1, 1, 0)
        && !valid(
            1, 1, kernel::sched::SchedulingContext::max_refills + 1)
        && kernel::sched::Urgency::make(
            kernel::sched::Urgency::level_count - 1)
        && !kernel::sched::Urgency::make(
            kernel::sched::Urgency::level_count);
}

bool test_object_store_unpublished_construction_rolls_back(
    const TestContext&) noexcept {
    SchedStorageGuard storage{};
    if (!storage.initialize()) {
        return false;
    }
    // KernelVSpace owns mapped guarded stack slots as a reusable pool. Warm
    // one slot so this test measures Thread construction rollback, not pool
    // growth.
    {
        auto warm = kernel::KernelStack::create(*sched_test_kernel);
        if (!warm) {
            return false;
        }
    }
    const usize free_before = sched_test_pmm->free_page_count();
    {
        auto stack = kernel::KernelStack::create(*sched_test_kernel);
        if (!stack) {
            return false;
        }
        auto pending = sched_test_objects->create_thread(
            libk::move(stack).value(),
            kernel::ExecutionBinding::kernel(*sched_test_kernel),
            kernel::Thread::KernelStart{unused_thread_entry, nullptr});
        if (!pending) {
            return false;
        }
        // Dropping Pending is the only rollback authority. No ObjectId exists
        // until publish(), so the half-constructed object is unaddressable.
    }
    return sched_test_pmm->free_page_count() == free_before
        && sched_test_pmm->verify_invariants();
}

bool test_kernel_stack_uses_guarded_virtual_slot(
    const TestContext&) noexcept {
    SchedStorageGuard storage{};
    if (!storage.initialize()) {
        return false;
    }

    usize first_base{};
    {
        auto created = kernel::KernelStack::create(*sched_test_kernel);
        if (!created) {
            return false;
        }
        auto stack = libk::move(created).value();
        first_base = stack.base();

        auto edit_result = sched_test_kernel->begin_edit();
        if (!edit_result) {
            return false;
        }
        auto edit = libk::move(edit_result).value();
        const auto lower = kernel::mm::VPage::from_base(
            kernel::mm::VirtAddr{stack.lower_guard()});
        const auto first = kernel::mm::VPage::from_base(
            kernel::mm::VirtAddr{stack.base()});
        const auto last = kernel::mm::VPage::from_base(
            kernel::mm::VirtAddr{stack.top() - kernel::mm::page_size});
        const auto upper = kernel::mm::VPage::from_base(
            kernel::mm::VirtAddr{stack.upper_guard()});
        if (!lower || !first || !last || !upper) {
            return false;
        }
        const auto lower_entry = edit.pages().query(*lower);
        const auto first_entry = edit.pages().query(*first);
        const auto last_entry = edit.pages().query(*last);
        const auto upper_entry = edit.pages().query(*upper);
        if (lower_entry
            || lower_entry.error() != arch::PageEditError::NotMapped
            || !first_entry
            || !last_entry
            || upper_entry
            || upper_entry.error() != arch::PageEditError::NotMapped
            || stack.size() != kernel::mm::KernelStackLayout::StackBytes) {
            return false;
        }
    }

    auto reused = kernel::KernelStack::create(*sched_test_kernel);
    return reused && reused.value().base() == first_base;
}

bool test_domain_admission_is_conservative_and_transactional(
    const TestContext&) noexcept {
    SchedStorageGuard storage{};
    if (!storage.initialize()) {
        return false;
    }
    auto capacity = kernel::sched::DomainCapacity::create(
        *sched_test_pmm, 1);
    if (!capacity) {
        return false;
    }
    auto pending_domain = sched_test_objects->create_domain(
        libk::move(capacity).value(),
        kernel::sched::SchedulingDomain::share_scale,
        100'000U);
    if (!pending_domain) {
        return false;
    }
    auto domain = libk::move(pending_domain).value().publish();

    kernel::object::ObjectStore::SchedulingContextHold contexts[3]{};
    constexpr u64 budgets[3]{1, 1, 1};
    constexpr u64 periods[3]{3, 3, 2};
    for (usize index = 0; index < 3; ++index) {
        auto pending = sched_test_objects->create_context(
            kernel::sched::SchedulingContext::Config{
                .budget = Duration::from_ticks(budgets[index]),
                .period = Duration::from_ticks(periods[index]),
            },
            Instant::from_ticks(0));
        if (!pending) {
            return false;
        }
        contexts[index] = libk::move(pending).value().publish();
    }

    const bool first = static_cast<bool>(
        domain->admit(contexts[0].get(), kernel::CpuId{0}));
    const bool second = static_cast<bool>(
        domain->admit(contexts[1].get(), kernel::CpuId{0}));
    const auto rejected = domain->admit(contexts[2].get(), kernel::CpuId{0});
    const bool rejected_cleanly = !rejected
        && rejected.error()
            == kernel::sched::SchedulingDomain::Error::CapacityExceeded
        && !contexts[2]->admitted();
    const bool released = static_cast<bool>(
        domain->unadmit(contexts[0].get()));
    const bool admitted_after_release = static_cast<bool>(
        domain->admit(contexts[2].get(), kernel::CpuId{0}));
    const auto wrong_cpu = domain->admit(contexts[0].get(), kernel::CpuId{1});

    const bool result = first && second && rejected_cleanly && released
        && admitted_after_release && !wrong_cpu
        && wrong_cpu.error()
            == kernel::sched::SchedulingDomain::Error::InvalidCpu;

    for (usize index = 1; index < 3; ++index) {
        if (!domain->unadmit(contexts[index].get())) {
            return false;
        }
    }
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
    sched_test_objects->drain_reclaim();
    return result && sched_test_pmm->verify_invariants();
}

bool test_ready_queue_orders_priority_and_fifo(
    const TestContext&) noexcept {
    SchedStorageGuard storage{};
    if (!storage.initialize()) {
        return false;
    }
    auto capacity = kernel::sched::DomainCapacity::create(
        *sched_test_pmm, 1);
    if (!capacity) {
        return false;
    }
    auto pending_domain = sched_test_objects->create_domain(
        libk::move(capacity).value(),
        kernel::sched::SchedulingDomain::share_scale,
        0U);
    if (!pending_domain) {
        return false;
    }
    auto domain = libk::move(pending_domain).value().publish();

    kernel::object::ObjectStore::ThreadHold threads[3]{};
    kernel::object::ObjectStore::SchedulingContextHold contexts[3]{};
    constexpr u8 levels[3]{2, 5, 5};
    for (usize index = 0; index < 3; ++index) {
        auto stack = kernel::KernelStack::create(*sched_test_kernel);
        if (!stack) {
            return false;
        }
        auto pending_thread = sched_test_objects->create_thread(
            libk::move(stack).value(),
            kernel::ExecutionBinding::kernel(*sched_test_kernel),
            kernel::Thread::KernelStart{unused_thread_entry, nullptr});
        const auto urgency = kernel::sched::Urgency::make(levels[index]);
        if (!pending_thread || !urgency) {
            return false;
        }
        threads[index] = libk::move(pending_thread).value().publish();

        auto pending_context = sched_test_objects->create_context(
            kernel::sched::SchedulingContext::Config{
                .budget = Duration::from_ticks(1),
                .period = Duration::from_ticks(10),
                .urgency = *urgency,
            },
            Instant::from_ticks(0));
        if (!pending_context) {
            return false;
        }
        contexts[index] = libk::move(pending_context).value().publish();
        auto target = threads[index].clone();
        if (!target
            || !domain->admit(contexts[index].get(), kernel::CpuId{0})
            || !contexts[index]->bind(
                libk::move(target).value(), kernel::CpuId{0})) {
            return false;
        }
    }

    kernel::sched::ReadyQueue queue{};
    kernel::sched::Binding* const low = contexts[0]->binding();
    kernel::sched::Binding* const high_first = contexts[1]->binding();
    kernel::sched::Binding* const high_second = contexts[2]->binding();
    const auto low_urgency = contexts[0]->urgency();
    const auto high_urgency = contexts[1]->urgency();
    queue.enqueue(*low, low_urgency);
    queue.enqueue(*high_first, high_urgency);
    queue.enqueue(*high_second, high_urgency);

    const bool ordered = queue.size() == 3
        && queue.front() == high_first
        && queue.pop_front(high_urgency) == high_first
        && queue.front() == high_second
        && queue.pop_front(high_urgency) == high_second
        && queue.front() == low
        && queue.pop_front(low_urgency) == low
        && queue.empty();
    queue.enqueue(*low, low_urgency);
    queue.remove(*low, low_urgency);
    const bool membership = queue.empty() && !low->queued();

    for (usize index = 0; index < 3; ++index) {
        if (!contexts[index]->unbind()
            || !domain->unadmit(contexts[index].get())
            || !contexts[index].retire()
            || !threads[index].retire()) {
            return false;
        }
        contexts[index].reset();
        threads[index].reset();
    }
    if (!domain.retire()) {
        return false;
    }
    domain.reset();
    sched_test_objects->drain_reclaim();
    return ordered && membership && sched_test_pmm->verify_invariants();
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
        "ObjectStore unpublished construction rolls back slab and payload",
        test_object_store_unpublished_construction_rolls_back);
    (void)registry.add(
        "sched",
        "kernel stacks use guarded reusable virtual slots",
        test_kernel_stack_uses_guarded_virtual_slot);
    (void)registry.add(
        "sched",
        "domain admission rounds conservatively and rolls back failure",
        test_domain_admission_is_conservative_and_transactional);
    (void)registry.add(
        "sched",
        "ReadyQueue chooses highest urgency and preserves FIFO",
        test_ready_queue_orders_priority_and_fifo);
}
