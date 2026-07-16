#include <test/test.hpp>

#include <cpu/cpu_set.hpp>
#include <mm/translation.hpp>

namespace {

bool test_activation_before_mutation_is_in_target_snapshot(
    const TestContext&) noexcept {
    kernel::mm::TranslationState state{};
    constexpr kernel::CpuId cpu{0};
    if (state.enter(cpu).raw != 0) {
        return false;
    }

    kernel::mm::ShootdownTicket ticket{};
    auto mutation = state.begin();
    if (!mutation) {
        state.leave(cpu);
        return false;
    }
    if (!mutation.value().targets().contains(cpu)) {
        mutation.value().abort();
        state.leave(cpu);
        return false;
    }
    auto plan = kernel::mm::ShootdownPlan::local(cpu, mutation.value().targets());
    if (!plan) {
        mutation.value().abort();
        state.leave(cpu);
        return false;
    }
    const auto status = mutation.value().commit(
        libk::move(plan).value(), ticket);
    const bool valid = status == kernel::mm::ShootdownStatus::Complete
        && ticket.complete()
        && ticket.epoch() == kernel::mm::TranslationEpoch{1}
        && ticket.targets().contains(cpu)
        && ticket.acknowledged(cpu);
    state.leave(cpu);
    return valid;
}

bool test_mutation_before_activation_publishes_new_epoch(
    const TestContext&) noexcept {
    kernel::mm::TranslationState state{};
    constexpr kernel::CpuId cpu{0};
    kernel::mm::ShootdownTicket ticket{};
    auto mutation = state.begin();
    if (!mutation || !mutation.value().targets().empty()) {
        return false;
    }
    auto plan = kernel::mm::ShootdownPlan::local(cpu, mutation.value().targets());
    if (!plan
        || mutation.value().commit(libk::move(plan).value(), ticket)
            != kernel::mm::ShootdownStatus::Complete
        || !ticket.complete()) {
        return false;
    }
    const kernel::mm::TranslationEpoch observed = state.enter(cpu);
    state.leave(cpu);
    return observed == ticket.epoch()
        && observed == kernel::mm::TranslationEpoch{1};
}

bool test_abort_and_distinct_mutations_preserve_versioning(
    const TestContext&) noexcept {
    kernel::mm::TranslationState state{};
    {
        auto aborted = state.begin();
        if (!aborted) {
            return false;
        }
    }
    if (state.epoch().raw != 0) {
        return false;
    }

    constexpr kernel::CpuId cpu{0};
    kernel::mm::ShootdownTicket first{};
    kernel::mm::ShootdownTicket second{};
    kernel::mm::ShootdownTicket* const tickets[] = {&first, &second};
    for (kernel::mm::ShootdownTicket* ticket : tickets) {
        auto mutation = state.begin();
        if (!mutation) {
            return false;
        }
        auto plan = kernel::mm::ShootdownPlan::local(
            cpu, mutation.value().targets());
        if (!plan
            || mutation.value().commit(libk::move(plan).value(), *ticket)
                != kernel::mm::ShootdownStatus::Complete) {
            return false;
        }
    }
    return first.complete()
        && second.complete()
        && first.epoch() == kernel::mm::TranslationEpoch{1}
        && second.epoch() == kernel::mm::TranslationEpoch{2}
        && state.epoch() == second.epoch();
}

} // namespace

void register_translation_tests(TestRegistry& registry) noexcept {
    (void)registry.add(
        "translation",
        "activation before mutation is captured by the ticket",
        test_activation_before_mutation_is_in_target_snapshot);
    (void)registry.add(
        "translation",
        "mutation before activation publishes the entering epoch",
        test_mutation_before_activation_publishes_new_epoch);
    (void)registry.add(
        "translation",
        "aborts do not advance and tickets retain distinct epochs",
        test_abort_and_distinct_mutations_preserve_versioning);
}
