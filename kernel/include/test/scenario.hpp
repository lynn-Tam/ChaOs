#pragma once

#include <boot/boot_info.hpp>
#include <diag/scenario.hpp>

namespace kernel {
struct CpuRuntime;
}

namespace kernel::test::scenario {

// Test-only runtime scenario entry. Production/off images link no
// implementation; the image selector remains a tiny typed value.
[[nodiscard]] auto run(
    diag::scenario::Id selected,
    const boot::BootInfo& boot) noexcept -> bool;
[[nodiscard]] auto run_runtime(
    diag::scenario::Id selected,
    CpuRuntime& runtime) noexcept -> bool;

namespace detail {
[[nodiscard]] auto remote(CpuRuntime& runtime) noexcept -> bool;
[[nodiscard]] auto publication(CpuRuntime& runtime) noexcept -> bool;
[[nodiscard]] auto resource_watchdog(
    CpuRuntime& runtime,
    bool require_report) noexcept -> bool;
[[nodiscard]] auto report(CpuRuntime& runtime) noexcept -> bool;
[[nodiscard]] auto ordinary(const boot::BootInfo& boot) noexcept -> bool;
[[nodiscard]] auto initrd(const boot::BootInfo& boot) noexcept -> bool;
[[nodiscard]] auto trap(CpuRuntime& runtime) noexcept -> bool;
[[nodiscard]] auto dispatch(CpuRuntime& runtime) noexcept -> bool;
} // namespace detail

} // namespace kernel::test::scenario
