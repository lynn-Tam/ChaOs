#pragma once

#include <core/types.hpp>

namespace kernel::boot {

[[noreturn]] void enter(
    usize hardware_id,
    usize fdt_physical,
    const void* fdt_view) noexcept;

} // namespace kernel::boot
