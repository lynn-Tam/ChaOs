#include <boot/info.hpp>
#include <libk/assert.hpp>
#include <base/types.hpp>
#include <boot/start.hpp>
#include <libk/manual_lifetime.hpp>
#include <mm/table.hpp>

namespace {

// The selected architecture owns the firmware-facing handoff storage.  Init
// moves the completed BootInfo into its own one-shot state and destroys this
// source before continuing beyond the architecture boot boundary.
constinit libk::ManualLifetime<BootInfo> boot_info_storage{};
constinit libk::ManualLifetime<mm::RegionList> memory_storage{};

} // namespace

extern "C" [[noreturn]] void arch_riscv64_high_entry_cpp(
    usize hardware_id,
    usize fdt_physical) noexcept {
    libk_assert(fdt_physical < mm::DirectSize);
    auto& boot_info = boot_info_storage.emplace();
    const auto built = build_boot_info_from_fdt(
        boot_info, memory_storage.emplace(),
        CpuHwId{hardware_id},
        mm::Phys{fdt_physical},
        reinterpret_cast<const void*>(
            mm::DirectBegin + fdt_physical));
    libk_assert(built);
    start_kernel(boot_info_storage, memory_storage);
}
