#include <expected>
#include <arch/ipi.hpp>

#include "arch/riscv64/cpu/csr.hpp"
#include <sbi.hpp>

#include <libk/sync/atomic.hpp>

namespace arch {
namespace {

libk::Atomic<usize> injected_failures{};

[[nodiscard]] auto consume_injected_failure() noexcept -> bool {
    usize remaining = injected_failures.load<libk::MemoryOrder::Acquire>();
    while (remaining != 0) {
        if (injected_failures.compare_exchange_weak<libk::MemoryOrder::AcqRel, libk::MemoryOrder::Acquire>(
                remaining, remaining - 1)) {
            return true;
        }
    }
    return false;
}

} // namespace

auto ipi_available() noexcept -> bool { return sbi::probe(sbi::Ext::Ipi); }

auto send_ipi(CpuHwId target) noexcept -> std::expected<void, IpiError> {
    if (consume_injected_failure()) {
        return std::unexpected(IpiError::Rejected);
    }
    constexpr usize width = sizeof(usize) * 8;
    const usize base = target.raw & ~(width - 1);
    const auto result = sbi::call(sbi::Ext::Ipi, 0, usize{1} << (target.raw - base), base);
    if (result) return {};
    if (result.error() == sbi::Unsupported) return std::unexpected(IpiError::NotSupported);
    if (result.error() == sbi::Invalid) return std::unexpected(IpiError::InvalidTarget);
    return std::unexpected(IpiError::Rejected);
}

void enable_ipi() noexcept { riscv64::Sie::enable_software(); }

void acknowledge_ipi() noexcept { riscv64::Sip::clear_software_pending(); }

void inject_ipi_failures_for_test(usize count) noexcept {
    injected_failures.store<libk::MemoryOrder::Release>(count);
}

} // namespace arch
