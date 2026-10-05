#include <test/scenario.hpp>

namespace test::scenario {

auto run(
    Id selected,
    const BootInfo& boot) noexcept -> bool {
    switch (selected) {
    case Id::Off:
        return true;
    case Id::Ordinary:
        return ordinary(boot);
    case Id::Initrd:
        return initrd(boot);
    case Id::Trap:
        // Trap entry/exit needs a published CPU runtime. The boot hook only
        // validates the selector; the runtime hook drives the real path.
        static_cast<void>(boot);
        return true;
    case Id::RemoteDelivery:
        static_cast<void>(boot);
        return true;
    case Id::Dispatch:
    case Id::IoLease:
    case Id::WaitPublication:
        // These scenarios need a published CpuRuntime and run from the
        // runtime hook below. Selection itself is validated before bring-up.
        return true;
    }
    return false;
}

auto run_runtime(
    Id selected,
    CpuRuntime& runtime) noexcept -> bool {
    switch (selected) {
    case Id::WaitPublication:
        return wait_publication(runtime);
    case Id::IoLease:
        return io_lease(runtime);
    case Id::Dispatch:
        return dispatch(runtime);
    case Id::Trap:
        return trap(runtime);
    case Id::RemoteDelivery:
        return remote(runtime);
    case Id::Off:
    case Id::Ordinary:
    case Id::Initrd:
        return true;
    }
    return false;
}



} // namespace test::scenario
