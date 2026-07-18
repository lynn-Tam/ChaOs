#pragma once
#include <boot/boot_info.hpp>
#include <libk/manual_lifetime.hpp>

namespace kernel::init {

[[noreturn]] void run(
    libk::ManualLifetime<kernel::boot::BootInfo>& source) noexcept;

} // namespace kernel::init
