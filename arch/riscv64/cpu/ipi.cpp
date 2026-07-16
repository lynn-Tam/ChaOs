#include <arch/ipi.hpp>

#include "arch/riscv64/cpu/csr.hpp"
#include "arch/riscv64/sbi/base.hpp"
#include "arch/riscv64/sbi/ipi.hpp"

#include <libk/sync/atomic.hpp>

namespace arch {
namespace {

libk::Atomic<usize> injected_failures{};

[[nodiscard]] auto consume_injected_failure() noexcept -> bool {
    usize remaining = injected_failures.load<libk::MemoryOrder::Acquire>();
    while (remaining != 0) {
        if (injected_failures.compare_exchange_weak<
                libk::MemoryOrder::AcqRel,
                libk::MemoryOrder::Acquire>(remaining, remaining - 1)) {
            return true;
        }
    }
    return false;
}

} // namespace

auto ipi_available() noexcept -> bool {
    return riscv64::sbi::extension_available(
        riscv64::sbi::ipi_extension_id);
}

auto send_ipi(kernel::CpuHardwareId target) noexcept
    -> libk::Expected<void, IpiError> {
    if (consume_injected_failure()) {
        return libk::unexpected(IpiError::Rejected);
    }
    const riscv64::sbi::HartMask mask =
        riscv64::sbi::single_hart_mask(target.raw);
    const riscv64::sbi::Ret result = riscv64::sbi::send_ipi(
        mask.bits, mask.base);
    if (result.error == riscv64::sbi::success) {
        return libk::expected();
    }
    if (result.error == riscv64::sbi::not_supported) {
        return libk::unexpected(IpiError::NotSupported);
    }
    if (result.error == riscv64::sbi::invalid_parameter) {
        return libk::unexpected(IpiError::InvalidTarget);
    }
    return libk::unexpected(IpiError::Rejected);
}

void enable_ipi() noexcept {
    riscv64::Sie::enable_software();
}

void acknowledge_ipi() noexcept {
    riscv64::Sip::clear_software_pending();
}

void inject_ipi_failures_for_test(usize count) noexcept {
    injected_failures.store<libk::MemoryOrder::Release>(count);
}

} // namespace arch
