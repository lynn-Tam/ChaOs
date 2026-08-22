#include <test/boot.hpp>
#include <test/scenario.hpp>

namespace kernel::test {

void run(const kernel::boot::BootInfo&) noexcept {}
void runtime(kernel::CpuRuntime&) noexcept {}

auto scenario::page_fault(
    CpuRuntime&, mm::VirtAddr) noexcept -> bool {
    return true;
}

} // namespace kernel::test
