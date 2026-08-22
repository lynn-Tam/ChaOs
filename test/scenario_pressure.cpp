#include <test/scenario.hpp>

#include <core/kernel_state.hpp>
#include <cpu/cpu_runtime.hpp>
#include <diag/console.hpp>
#include <libk/utility.hpp>
#include <libk/sync/atomic.hpp>
#include <mm/pmm.hpp>

/*luna change: implement the pressure-only frame hold at the existing runtime hook, reason: the fixture must establish capacity without adding a kernel or proof control path*/
namespace kernel::test::scenario::detail {
namespace {

/*luna change: fix the pressure fixture floor at one policy value, reason: the PMM free count remains canonical and the fixture must not adapt a shadow threshold*/
constexpr usize StressAddress = MYOS_TEST_PRESSURE_STRESS_ADDRESS;

/*luna change: retain a fixed PMM group only in the pressure image, reason: the proof needs physical capacity pressure without a second allocator truth*/
// This owner remains engaged through every production proof barrier. The
// scenario's post-proof release-address fault is its only normal release edge.
constinit libk::ManualLifetime<mm::OwnedPageGroup> pressure_hold{};
constinit libk::Atomic<bool> pressure_triggered{};

} // namespace

/*luna change: arm pressure after root admission, reason: the real stress fault
  rather than runtime setup owns the fixture's PMM transition*/
auto pressure(CpuRuntime& runtime) noexcept -> bool {
    if (runtime.kernel == nullptr || pressure_hold
        || pressure_triggered.load<libk::MemoryOrder::Acquire>()) {
        return false;
    }
    diag::console::print<"[scenario] pressure armed free={}\n">(
        runtime.kernel->pmm().free_page_count());
    return true;
}

auto page_fault(CpuRuntime& runtime, mm::VirtAddr address) noexcept -> bool {
    if (runtime.kernel == nullptr) {
        return false;
    }
    if (address.raw() == MYOS_TEST_PRESSURE_RELEASE_ADDRESS) {
        if (!pressure_triggered.load<libk::MemoryOrder::Acquire>()
            || !pressure_hold) {
            return false;
        }
        const usize held = pressure_hold->page_count();
        // The three production barriers are complete before userspace can
        // touch this lazy page. Release precedes construction of its PageFault,
        // so these frames cannot satisfy any relation used by the proof.
        pressure_hold.reset();
        diag::console::print<
            "[scenario] pressure released held={} free={}\n">(
            held, runtime.kernel->pmm().free_page_count());
        return true;
    }
    if (address.raw() != StressAddress) {
        return true;
    }
    bool expected{};
    if (!pressure_triggered.compare_exchange_strong<
            libk::MemoryOrder::AcqRel,
            libk::MemoryOrder::Acquire>(expected, true)) {
        return true;
    }
    mm::Pmm& pmm = runtime.kernel->pmm();
    const usize free = pmm.free_page_count();
    auto group = pmm.make_page_group();
    if (!group.try_extend(free)) {
        diag::console::print<"[scenario] pressure setup failure free={}\n">(
            free);
        return false;
    }
    auto& held = pressure_hold.emplace(libk::move(group));
    // The fourth lazy page cannot fault until all three proof barriers, so no
    // tested pressure relation can observe this owner's eventual release.
    diag::console::print<"[scenario] pressure drained held={} free={}\n">(
        held.page_count(), pmm.free_page_count());
    return true;
}

} // namespace kernel::test::scenario::detail
