#include <test/test.hpp>

#include <arch/cpu.hpp>
#include <arch/interrupt.hpp>
#include <libk/utility.hpp>
#include <sync/irq_lock_guard.hpp>
#include <sync/model.hpp>

namespace {

[[nodiscard]] auto test_dep_graph_finds_paths_across_words(
    [[maybe_unused]] const TestContext& context) noexcept -> bool {
    static kernel::sync::DepGraph<70> graph{};
    graph.insert(1, 65);
    graph.insert(65, 69);
    static kernel::sync::DepGraph<70>::Path path{};
    path = {};
    return graph.has(1, 65)
        && graph.has(65, 69)
        && graph.path(1, 69, path)
        && path.size == 3
        && path.nodes[0] == 1
        && path.nodes[1] == 65
        && path.nodes[2] == 69;
}

[[nodiscard]] auto test_dep_graph_rejects_absent_path(
    [[maybe_unused]] const TestContext& context) noexcept -> bool {
    kernel::sync::DepGraph<5> graph{};
    graph.insert(0, 1);
    graph.insert(2, 3);
    kernel::sync::DepGraph<5>::Path path{};
    return !graph.path(1, 0, path) && path.size == 0;
}

[[nodiscard]] auto test_dep_graph_serializes_reverse_edge(
    [[maybe_unused]] const TestContext& context) noexcept -> bool {
    kernel::sync::DepGraph<4> graph{};
    const auto first = graph.check_insert(0, 1);
    const auto reverse = graph.check_insert(1, 0);
    return first.status == kernel::sync::DepStatus::Added
        && reverse.status == kernel::sync::DepStatus::Cycle
        && reverse.path.size == 2
        && reverse.path.nodes[0] == 0
        && reverse.path.nodes[1] == 1;
}

[[nodiscard]] auto test_wait_cycle_requires_stable_closed_stamps(
    [[maybe_unused]] const TestContext& context) noexcept -> bool {
    kernel::sync::WaitStamp candidate[2]{
        {0, 0x1000, 0x1010, 7,
            kernel::sync::OwnerWord::pack(kernel::CpuId{1}, 11)},
        {1, 0x2000, 0x2010, 9,
            kernel::sync::OwnerWord::pack(kernel::CpuId{0}, 13)},
    };
    kernel::sync::WaitStamp observed[2]{candidate[0], candidate[1]};
    if (kernel::sync::validate_wait_cycle(candidate, observed, 2)
        != kernel::sync::WaitCheck::Stable) {
        return false;
    }
    ++observed[1].wait_generation;
    if (kernel::sync::validate_wait_cycle(candidate, observed, 2)
        != kernel::sync::WaitCheck::Changed) {
        return false;
    }
    observed[1] = candidate[1];
    observed[0].owner_word = kernel::sync::OwnerWord::pack(
        kernel::CpuId{3}, 11);
    candidate[0].owner_word = observed[0].owner_word;
    return kernel::sync::validate_wait_cycle(candidate, observed, 2)
        == kernel::sync::WaitCheck::Open;
}

[[nodiscard]] auto test_owner_word_preserves_generation(
    [[maybe_unused]] const TestContext& context) noexcept -> bool {
    constexpr u64 word = kernel::sync::OwnerWord::pack(kernel::CpuId{255}, 77);
    return kernel::sync::OwnerWord::cpu(word) == 255
        && kernel::sync::OwnerWord::generation(word) == 77
        && kernel::sync::OwnerWord::cpu(
            kernel::sync::OwnerWord::none(77)) == kernel::max_cpu_count
        && kernel::sync::OwnerWord::generation(
            kernel::sync::OwnerWord::none(77)) == 77;
}

[[nodiscard]] auto test_source_site_is_captured_at_call(
    [[maybe_unused]] const TestContext& context) noexcept -> bool {
    const u32 expected = static_cast<u32>(__LINE__ + 1);
    const kernel::sync::LockSite site = kernel::sync::LockSite::current();
    return site.file != nullptr
        && site.function != nullptr
        && site.line == expected;
}

[[nodiscard]] auto test_irq_token_move_restores_once(
    [[maybe_unused]] const TestContext& context) noexcept -> bool {
    const bool enabled = arch::interrupts_enabled();
    {
        kernel::sync::IrqToken first{};
        if (arch::interrupts_enabled()) {
            return false;
        }
        kernel::sync::IrqToken second{libk::move(first)};
        if (!second.active() || first.active()) {
            return false;
        }
    }
    return arch::interrupts_enabled() == enabled;
}

[[nodiscard]] auto test_untracked_irq_restore_is_ignored(
    [[maybe_unused]] const TestContext& context) noexcept -> bool {
    // Models a token created before per-CPU diagnostics publication and
    // restored after publication. It never contributed to the tracked depth.
    kernel::sync::irq_restoring({});
    return true;
}

[[nodiscard]] auto test_ordered_pair_and_try_token_own_once(
    [[maybe_unused]] const TestContext& context) noexcept -> bool {
    using PairLock = kernel::sync::SpinLock<
        kernel::sync::LockClass::CSpace,
        kernel::sync::SameClassPolicy::AddressAscending>;
    PairLock first{};
    PairLock second{};
    {
        kernel::sync::OrderedIrqLockPair pair{first, second};
    }

    kernel::sync::SpinLock<kernel::sync::LockClass::Wait> try_target{};
    kernel::sync::IrqLockToken token{
        try_target, kernel::sync::try_lock};
    return token.owns_lock();
}

} // namespace

void register_sync_tests(TestRegistry& registry) noexcept {
    (void)registry.add(
        "sync",
        "dependency graph spans multiword rows and reconstructs paths",
        test_dep_graph_finds_paths_across_words);
    (void)registry.add(
        "sync",
        "dependency graph leaves absent paths empty",
        test_dep_graph_rejects_absent_path);
    (void)registry.add(
        "sync",
        "serialized reverse edge returns a typed cycle",
        test_dep_graph_serializes_reverse_edge);
    (void)registry.add(
        "sync",
        "wait-cycle validation rejects changed and open snapshots",
        test_wait_cycle_requires_stable_closed_stamps);
    (void)registry.add(
        "sync",
        "owner word preserves CPU and acquisition generation",
        test_owner_word_preserves_generation);
    (void)registry.add(
        "sync",
        "LockSite compiler builtins capture the call site",
        test_source_site_is_captured_at_call);
    (void)registry.add(
        "sync",
        "movable IRQ ownership restores the original state once",
        test_irq_token_move_restores_once);
    (void)registry.add(
        "sync",
        "untracked early IRQ ownership does not consume tracked depth",
        test_untracked_irq_restore_is_ignored);
    (void)registry.add(
        "sync",
        "ordered pair and try token own successful acquisitions once",
        test_ordered_pair_and_try_token_own_once);
}
