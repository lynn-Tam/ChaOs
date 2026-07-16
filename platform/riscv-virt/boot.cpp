#include <arch/address_layout.hpp>
#include <boot/entry.hpp>
#include <core/debug.hpp>

extern "C" [[noreturn]] void platform_riscv_virt_high_entry(
    usize hardware_id,
    usize fdt_physical) noexcept {
    KASSERT(fdt_physical < arch::layout::direct_size);
    kernel::boot::enter(
        hardware_id,
        fdt_physical,
        reinterpret_cast<const void*>(
            arch::layout::direct_base + fdt_physical));
}
