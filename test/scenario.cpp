#include <test/scenario.hpp>

namespace kernel::test::scenario {

auto run(
    diag::scenario::Id selected,
    const boot::BootInfo& boot) noexcept -> bool {
    switch (selected) {
    case diag::scenario::Id::Off:
        return true;
    case diag::scenario::Id::Ordinary:
        return detail::ordinary(boot);
    case diag::scenario::Id::Initrd:
        return detail::initrd(boot);
    case diag::scenario::Id::Trap:
        // Trap entry/exit needs a published CPU runtime. The boot hook only
        // validates the selector; the runtime hook drives the real path.
        static_cast<void>(boot);
        return true;
    case diag::scenario::Id::RemoteDelivery:
        static_cast<void>(boot);
        return true;
    case diag::scenario::Id::Publication:
    case diag::scenario::Id::ReportRetry:
    case diag::scenario::Id::Dispatch:
    case diag::scenario::Id::Observer:
    /*luna change: keep pressure selection runtime-only, reason: frame holding must occur after root-pool admission rather than during boot validation*/
    case diag::scenario::Id::Pressure:
        // These scenarios need a published CpuRuntime and run from the
        // runtime hook below. Selection itself is validated before bring-up.
        return true;
    }
    return false;
}

auto run_runtime(
    diag::scenario::Id selected,
    CpuRuntime& runtime) noexcept -> bool {
    switch (selected) {
    case diag::scenario::Id::Publication:
        return detail::publication(runtime);
    case diag::scenario::Id::ReportRetry:
        return detail::report(runtime);
    case diag::scenario::Id::Dispatch:
        return detail::dispatch(runtime);
    case diag::scenario::Id::Observer:
        return detail::observer(runtime);
    /*luna change: route pressure selection through the existing runtime hook, reason: frame holding must happen after root admission and before user execution*/
    case diag::scenario::Id::Pressure:
        return detail::pressure(runtime);
    case diag::scenario::Id::Trap:
        return detail::trap(runtime);
    case diag::scenario::Id::RemoteDelivery:
        return detail::remote(runtime);
    case diag::scenario::Id::Off:
    case diag::scenario::Id::Ordinary:
    case diag::scenario::Id::Initrd:
        return true;
    }
    return false;
}

auto page_fault(CpuRuntime& runtime, mm::VirtAddr address) noexcept -> bool {
    if (diag::scenario::selected != diag::scenario::Id::Pressure) {
        return true;
    }
    return detail::page_fault(runtime, address);
}

} // namespace kernel::test::scenario
