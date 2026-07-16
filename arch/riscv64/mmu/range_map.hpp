// Construction-time range helpers for detached Sv39 builders.
// This is private RISC-V MMU support, not a public page-table contract.

#pragma once

#include "sv39_builder.hpp"

namespace arch::riscv64 {

// Maps a contiguous physical range at a contiguous virtual-page base.
//
// This helper intentionally stays outside the builder's public API: it is not
// atomic. If mapping page N fails, pages 0..N-1 may already be present in the
// builder. Callers must discard the builder on failure or prove that the mapped
// prefix is acceptable.
[[nodiscard]] auto map_range(
    Sv39Builder& builder,
    kernel::mm::VPage first_vpage,
    kernel::mm::PageRange physical,
    PtePerm permissions) noexcept -> Sv39Builder::MapResult;

// Checks one construction-time leaf mapping without mutating the builder.
[[nodiscard]] auto maps_page(
    const Sv39Builder& builder,
    kernel::mm::VPage vpage,
    kernel::mm::Page physical,
    PtePerm permissions) noexcept -> bool;

// Checks that a contiguous physical range is mapped at a contiguous virtual
// range with the same leaf permissions.
[[nodiscard]] auto maps_range(
    const Sv39Builder& builder,
    kernel::mm::VPage first_vpage,
    kernel::mm::PageRange physical,
    PtePerm permissions) noexcept -> bool;

} // namespace arch::riscv64
