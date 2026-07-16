#include <boot/entry.hpp>

#include <boot/boot_info.hpp>
#include <boot/run.hpp>
#include <core/debug.hpp>
#include <libk/manual_lifetime.hpp>
#include <libk/utility.hpp>

namespace kernel::boot {
namespace {

constinit libk::ManualLifetime<BootInfo> boot_info_storage{};

} // namespace

[[noreturn]] void enter(
    usize hardware_id,
    usize fdt_physical,
    const void* fdt_view) noexcept {
    BootInfo& boot_info = boot_info_storage.emplace();
    const auto built = build_boot_info_from_fdt(
        boot_info, kernel::mm::PhysAddr{fdt_physical}, fdt_view);
    KASSERT(built);
    run(libk::move(boot_info), CpuHardwareId{hardware_id});
    __builtin_unreachable();
}

} // namespace kernel::boot
