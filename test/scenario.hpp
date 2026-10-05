#pragma once

#include <boot/info.hpp>

#include <mm/types.hpp>

struct CpuRuntime;

namespace test::scenario {

enum class Id : u16 {
    Off = 0,
    Ordinary = 1,
    Initrd = 2,
    Trap = 3,
    RemoteDelivery = 4,
    Dispatch = 7,
    IoLease = 10,
    WaitPublication = 11,
};

extern const Id selected;

// Only test executables link these entries and their selected configuration.
[[nodiscard]] auto run(
    Id selected,
    const BootInfo& boot) noexcept -> bool;
[[nodiscard]] auto run_runtime(
    Id selected,
    CpuRuntime& runtime) noexcept -> bool;

[[nodiscard]] auto io_lease(CpuRuntime& runtime) noexcept -> bool;
[[nodiscard]] auto remote(CpuRuntime& runtime) noexcept -> bool;
[[nodiscard]] auto ordinary(const BootInfo& boot) noexcept -> bool;
[[nodiscard]] auto initrd(const BootInfo& boot) noexcept -> bool;
[[nodiscard]] auto trap(CpuRuntime& runtime) noexcept -> bool;
[[nodiscard]] auto dispatch(CpuRuntime& runtime) noexcept -> bool;
[[nodiscard]] auto wait_publication(CpuRuntime& runtime) noexcept -> bool;

} // namespace test::scenario
