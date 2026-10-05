#include <mm/tlb.hpp>
#include <test/test.hpp>

static bool active_snapshot(const TestContext &) noexcept {
    mm::Tlb tlb;
    constexpr CpuId cpu{0};
    tlb.enter(cpu);
    mm::Flush flush;
    auto edit = tlb.begin();
    const bool captured = edit.targets().contains(cpu);
    const bool complete = edit.commit(flush, nullptr, cpu, true);
    tlb.leave(cpu);
    return captured && complete && flush.complete() && flush.acknowledged(cpu) && flush.executable();
}

static bool empty_and_aborted(const TestContext &) noexcept {
    mm::Tlb tlb;
    {
        auto aborted = tlb.begin();
    }
    mm::Flush flush;
    auto edit = tlb.begin();
    if (!edit.targets().empty() || !edit.commit(flush, nullptr, CpuId{0})) return false;
    tlb.enter(CpuId{0});
    const bool valid = flush.targets().empty() && tlb.active_cpus().contains(CpuId{0});
    tlb.leave(CpuId{0});
    return valid && flush.complete();
}

void register_tlb_tests(TestRegistry &registry) noexcept {
    (void)registry.add("tlb", "active snapshot completes only after local fence", active_snapshot);
    (void)registry.add("tlb", "empty and aborted edits leave no remote obligations", empty_and_aborted);
}
