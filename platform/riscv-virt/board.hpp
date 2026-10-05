#pragma once

#include <expected>

#include "config.hpp"
#include <boot/info.hpp>
#include <io/device.hpp>
#include <object/pool.hpp>
#include <time/clock.hpp>

[[nodiscard]] auto virt_io_start(object::pool<io::Device>&, mm::Pmm&, const BootInfo&, const time::Clock&) noexcept -> bool;
[[nodiscard]] auto virt_uart_irq() noexcept -> irq::Line;
[[nodiscard]] auto virt_device_count() noexcept -> usize;
[[nodiscard]] auto virt_device(usize index = 0) noexcept -> io::Device&;
[[nodiscard]] auto virt_device_ref(usize index = 0) noexcept -> std::expected<object::ref<>, object::error>;

// Board resource inventory; CPU accesses retain platform PMAs.
[[nodiscard]] auto virt_mmio(const BootInfo&) noexcept -> std::array<mm::Region, 5>;

void virt_irq_start(usize hart) noexcept;
void virt_irq() noexcept;
