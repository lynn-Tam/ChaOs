#pragma once

#include <cpu/topology.hpp>
#include <libk/expected.hpp>

namespace arch {

enum class IpiError : u8 {
    NotSupported,
    InvalidTarget,
    Rejected,
};

[[nodiscard]] auto ipi_available() noexcept -> bool;
[[nodiscard]] auto send_ipi(kernel::CpuHardwareId target) noexcept
    -> libk::Expected<void, IpiError>;
void enable_ipi() noexcept;
void acknowledge_ipi() noexcept;

//Confirmatory experiment.
// Exit condition: remove when selected-arch transports have a pluggable test
// backend that can inject delivery failures without a production hook.
void inject_ipi_failures_for_test(usize count) noexcept;

} // namespace arch
