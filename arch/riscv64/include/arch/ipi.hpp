#pragma once

#include <cpu/types.hpp>
#include <expected>

namespace arch {

enum class IpiError : u8 {
    NotSupported,
    InvalidTarget,
    Rejected,
};

[[nodiscard]] auto ipi_available() noexcept -> bool;
[[nodiscard]] auto send_ipi(CpuHwId target) noexcept
    -> std::expected<void, IpiError>;
void enable_ipi() noexcept;
void acknowledge_ipi() noexcept;

//Confirmatory experiment.
// Exit condition: remove when selected-arch transports have a pluggable test
// backend that can inject delivery failures without a production hook.
void inject_ipi_failures_for_test(usize count) noexcept;

} // namespace arch
