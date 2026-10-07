#pragma once

#include <optional>
#include <mm/types.hpp>

// Image metadata is emitted by one image-specific translation unit. Keeping
// the build identity out of common objects lets compatible kernel/test/proof
// objects be reused when only the image label changes.
extern const char build_id[];

[[nodiscard]] auto kernel_begin() noexcept -> mm::Virt;
[[nodiscard]] auto kernel_end() noexcept -> mm::Virt;
[[nodiscard]] auto kernel_phys() noexcept -> mm::Phys;

[[nodiscard]] auto boot_pages() noexcept -> mm::Pages;
[[nodiscard]] auto secondary_pages() noexcept -> mm::Pages;
[[nodiscard]] auto transition_pages() noexcept -> mm::Pages;
[[nodiscard]] auto kernel_pages() noexcept -> mm::Pages;

// Converts only addresses inside the statically linked high kernel image.
// Runtime RAM translation remains owned by mm::Pmm.
[[nodiscard]] auto kernel_phys(mm::Virt address) noexcept
    -> std::optional<mm::Phys>;


extern "C" bool boot_guard_ok() noexcept;
