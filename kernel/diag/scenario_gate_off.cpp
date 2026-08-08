#include <diag/scenario.hpp>

namespace kernel::diag::scenario {

void report_retry_arm() noexcept {}

auto report_retry_evidence() noexcept -> ReportRetryEvidence {
    return {};
}

auto report_notify_gate() noexcept -> bool {
    return true;
}

} // namespace kernel::diag::scenario
