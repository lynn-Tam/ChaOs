#pragma once

#include <boot/info.hpp>

#include <mm/types.hpp>

struct Cpu;

namespace test::scenario {

enum class Id : u16 {
    Off = 0,
    Ordinary = 1,
    Initrd = 2,
    Trap = 3,
    RemoteDelivery = 4,
    Dispatch = 7,
    WaitPublication = 11,
};

extern const Id selected;

// Only test executables link these entries and their selected configuration.
[[nodiscard]] auto run(
    Id selected,
    const BootInfo& boot) noexcept -> bool;
[[nodiscard]] auto run_runtime(
    Id selected,
    Cpu& runtime) noexcept -> bool;

[[nodiscard]] auto remote(Cpu& runtime) noexcept -> bool;
[[nodiscard]] auto ordinary(const BootInfo& boot) noexcept -> bool;
[[nodiscard]] auto initrd(const BootInfo& boot) noexcept -> bool;
[[nodiscard]] auto trap(Cpu& runtime) noexcept -> bool;
[[nodiscard]] auto dispatch(Cpu& runtime) noexcept -> bool;
[[nodiscard]] auto wait_publication(Cpu& runtime) noexcept -> bool;

} // namespace test::scenario
