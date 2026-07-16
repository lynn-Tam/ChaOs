#include <boot/entry.hpp>
#include <core/debug.hpp>
#include <mm/virtual_layout.hpp>

extern "C" [[noreturn]] void arch_riscv64_high_entry_cpp(
    usize hardware_id,
    usize fdt_physical) noexcept {
    KASSERT(fdt_physical < kernel::mm::layout::DirectMapSize);
    kernel::boot::enter(
        hardware_id,
        fdt_physical,
        reinterpret_cast<const void*>(
            kernel::mm::layout::DirectMapBegin + fdt_physical));
}
