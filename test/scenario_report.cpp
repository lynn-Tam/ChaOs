#include <test/scenario.hpp>

#include <core/kernel_state.hpp>
#include <cpu/cpu_runtime.hpp>
#include <diag/console.hpp>
#include <diag/scenario.hpp>
#include <libk/utility.hpp>
#include <mm/kernel_stack.hpp>
#include <sched/context.hpp>
#include <sched/dispatcher.hpp>
#include <sync/irq_lock_guard.hpp>
#include <thread/thread.hpp>

namespace {

using namespace kernel;

void wake_entry(void*) noexcept {}

[[nodiscard]] auto wake_credit(CpuRuntime& runtime) noexcept -> bool {
    KernelState& kernel = *runtime.kernel;
    auto stack = KernelStack::create(kernel.kernel_vspace());
    if (!stack) {
        return false;
    }
    auto pending_thread = kernel.objects().create_thread(
        libk::move(stack).value(),
        ExecutionBinding::kernel(kernel.kernel_vspace()),
        Thread::KernelStart{wake_entry, nullptr});
    if (!pending_thread) {
        return false;
    }
    auto thread = libk::move(pending_thread).value().publish();
    const auto budget = kernel.clock().duration_from_nanoseconds(1'000'000);
    const auto period = kernel.clock().duration_from_nanoseconds(10'000'000);
    if (!budget || !period) {
        static_cast<void>(thread.retire());
        thread.reset();
        kernel.objects().drain_reclaim();
        return false;
    }
    auto pending_context = kernel.objects().create_context(
        sched::SchedulingContext::Config{.budget = *budget, .period = *period},
        kernel.clock().now());
    if (!pending_context) {
        static_cast<void>(thread.retire());
        thread.reset();
        kernel.objects().drain_reclaim();
        return false;
    }
    auto context = libk::move(pending_context).value().publish();
    const CpuId cpu = runtime.local.descriptor->logical_id();
    auto admitted = kernel.kernel_domain().admit(context.get(), cpu);
    auto target = thread.clone();
    if (!admitted || !target
        || !context->bind(libk::move(target).value())) {
        if (context->admitted()) {
            static_cast<void>(kernel.kernel_domain().unadmit(context.get()));
        }
        static_cast<void>(context.retire());
        context.reset();
        static_cast<void>(thread.retire());
        thread.reset();
        kernel.objects().drain_reclaim();
        return false;
    }
    sched::Binding* const binding = context->binding();
    bool accepted{};
    bool credit{};
    if (binding != nullptr) {
        sync::IrqToken irq{};
        accepted = runtime.dispatcher().accept_wake(*binding)
            == sched::CpuDispatcher::WakeAcceptance::Accepted;
        credit = binding->wake_credit();
    }
    const bool unbound = context->binding() == nullptr
        || static_cast<bool>(context->unbind());
    const bool unadmitted = static_cast<bool>(
        kernel.kernel_domain().unadmit(context.get()));
    const bool context_retired = context.retire();
    const bool thread_retired = thread.retire();
    context.reset();
    thread.reset();
    kernel.objects().drain_reclaim();
    return accepted && credit && unbound && unadmitted && context_retired
        && thread_retired;
}

} // namespace

namespace kernel::test::scenario::detail {

auto report(CpuRuntime& runtime) noexcept -> bool {
    diag::scenario::report_retry_arm();
    const bool delivered = resource_watchdog(runtime, true);
    const auto retry = diag::scenario::report_retry_evidence();
    const bool credit = wake_credit(runtime);
    const bool result = delivered
        && retry.first_failed
        && retry.attempts >= 2
        && retry.succeeded
        && credit;
    if (result) {
        diag::console::print<
            "[scenario] report-retry wake-credit ok "
            "retry-first-fail={} retry-attempts={} retry-success={} "
            "consumed={} wake-credit={}\n">(
            static_cast<u32>(retry.first_failed),
            retry.attempts,
            static_cast<u32>(retry.succeeded),
            static_cast<u32>(delivered),
            static_cast<u32>(credit));
    }
    return result;
}

} // namespace kernel::test::scenario::detail
