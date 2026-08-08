#pragma once

#include <core/types.hpp>

namespace kernel::diag::scenario {

// Scenario selection is an image-level input. Common kernel objects only
// observe this typed boundary; the selected provider is linked once per image.
// Values describe the mechanism under test. Historical numbered probes are
// translated by the build wrapper and never become part of this ABI.
enum class Id : u16 {
    Off = 0,
    Ordinary = 1,
    Initrd = 2,
    Trap = 3,
    RemoteDelivery = 4,
    Publication = 5,
    ReportRetry = 6,
    Dispatch = 7,
    Observer = 8,
};

extern const Id selected;

[[nodiscard]] auto enabled(Id value) noexcept -> bool;

// Scenario-owned report fault evidence crosses the diagnostic notifier at one
// typed out-of-line gate. Production/off providers always permit the wake;
// the report scenario provider rejects exactly its first armed attempt.
struct ReportRetryEvidence final {
    u32 attempts{};
    bool first_failed{};
    bool succeeded{};
};

void report_retry_arm() noexcept;
[[nodiscard]] auto report_retry_evidence() noexcept -> ReportRetryEvidence;
[[nodiscard]] auto report_notify_gate() noexcept -> bool;

} // namespace kernel::diag::scenario
