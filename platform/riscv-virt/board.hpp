#pragma once

#include <expected>

#include "config.hpp"
#include <boot/info.hpp>
#include <io/objects.hpp>
#include <time/clock.hpp>

[[nodiscard]] auto virt_io_start(io::objects&, mm::Pmm&, const BootInfo&, const time::Clock&) noexcept -> bool;
[[nodiscard]] auto virt_uart_irq() noexcept -> irq::Line;
[[nodiscard]] auto virt_device_count() noexcept -> usize;
[[nodiscard]] auto virt_device(usize index = 0) noexcept -> io::Device&;
[[nodiscard]] auto virt_device_ref(usize index = 0) noexcept -> std::expected<object::ref<>, object::error>;

// virt PMAs supply RAM/cache and device ordering; no PBMT override is requested.
[[nodiscard]] auto virt_mmio(const BootInfo&) noexcept -> std::array<mm::Region, 5>;
