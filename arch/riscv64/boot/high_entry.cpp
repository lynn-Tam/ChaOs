#include <boot/boot_info.hpp>
#include <core/debug.hpp>
#include <init/run.hpp>
#include <libk/manual_lifetime.hpp>
#include <mm/virtual_layout.hpp>

namespace {

// The selected architecture owns the firmware-facing handoff storage.  Init
// moves the completed BootInfo into its own one-shot state and destroys this
// source before continuing beyond the architecture boot boundary.
constinit libk::ManualLifetime<kernel::boot::BootInfo> boot_info_storage{};

} // namespace

extern "C" [[noreturn]] void arch_riscv64_high_entry_cpp(
    usize hardware_id,
    usize fdt_physical) noexcept {
    KASSERT(fdt_physical < kernel::mm::layout::DirectMapSize);
    auto& boot_info = boot_info_storage.emplace();
    const auto built = kernel::boot::build_boot_info_from_fdt(
        boot_info,
        kernel::CpuHardwareId{hardware_id},
        kernel::mm::PhysAddr{fdt_physical},
        reinterpret_cast<const void*>(
            kernel::mm::layout::DirectMapBegin + fdt_physical));
    KASSERT(built);
    kernel::init::run(boot_info_storage);
}
