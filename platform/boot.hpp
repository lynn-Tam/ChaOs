#pragma once
#include <boot/info.hpp>
#include <io/host.hpp>
#include <time/clock.hpp>
#include <uapi/start.h>
#include <variant>

// Hardware descriptions borrow machine-lifetime backends. Only the kernel
// creates capability objects and owns their references.
struct HwRes {
    BootCap entry;
    std::variant<io::Reg, irq::Line, io::Bus*> value;
};
struct BootHw {
    libk::InplaceVector<HwRes, (mm::page_size - sizeof(BootHdr)) / sizeof(BootCap)> resources;
    libk::delegate<void() noexcept> ext_irq;
};
auto platform_boot(BootInfo&, mm::RegionList&, CpuHwId, mm::Phys) noexcept -> bool;
auto platform_start(mm::Pmm&, const time::Clock&, BootHw&) noexcept -> bool;
