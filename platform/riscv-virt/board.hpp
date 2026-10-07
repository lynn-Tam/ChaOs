#pragma once

#include <expected>

#include <boot/info.hpp>
#include <io/device.hpp>
#include <object/pool.hpp>
#include <time/clock.hpp>

#include <cap/cap.hpp>
#include <irq/irq.hpp>
#include <mm/mem.hpp>
#include <libk/delegate.hpp>
#include <uapi/start.h>

[[nodiscard]] auto virt_io_start(object::pool<io::Device>&, object::pool<mm::Mem>&,
                                 object::pool<irq::Irq>&, mm::Pmm&, const BootInfo&,
                                 const time::Clock&) noexcept -> bool;
// The board retains identities; the receiver installs attenuated root grants.
using BootCaps = libk::delegate<bool(BootCap, object::ref<>&&, cap::View) noexcept>;
[[nodiscard]] auto virt_caps(BootCaps) noexcept -> bool;

// Board resource inventory; CPU accesses retain platform PMAs.
[[nodiscard]] auto virt_mmio(const BootInfo&) noexcept -> std::array<mm::Region, 5>;

void virt_irq_start(usize hart) noexcept;
void virt_irq() noexcept;
