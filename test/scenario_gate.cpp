#include <diag/scenario.hpp>

#include <libk/sync/atomic.hpp>

namespace kernel::diag::scenario {
namespace {

libk::Atomic<u32> armed{};
libk::Atomic<u32> attempts{};
libk::Atomic<u32> first_failed{};
libk::Atomic<u32> succeeded{};

} // namespace

void report_retry_arm() noexcept {
    attempts.store<libk::MemoryOrder::Release>(0);
    first_failed.store<libk::MemoryOrder::Release>(0);
    succeeded.store<libk::MemoryOrder::Release>(0);
    armed.store<libk::MemoryOrder::Release>(1);
}

auto report_retry_evidence() noexcept -> ReportRetryEvidence {
    return ReportRetryEvidence{
        attempts.load<libk::MemoryOrder::Acquire>(),
        first_failed.load<libk::MemoryOrder::Acquire>() != 0,
        succeeded.load<libk::MemoryOrder::Acquire>() != 0};
}

auto report_notify_gate() noexcept -> bool {
    if (armed.load<libk::MemoryOrder::Acquire>() == 0) {
        return true;
    }
    const u32 attempt = attempts.fetch_add<libk::MemoryOrder::AcqRel>(1);
    if (attempt == 0) {
        first_failed.store<libk::MemoryOrder::Release>(1);
        return false;
    }
    succeeded.store<libk::MemoryOrder::Release>(1);
    armed.store<libk::MemoryOrder::Release>(0);
    return true;
}

} // namespace kernel::diag::scenario
