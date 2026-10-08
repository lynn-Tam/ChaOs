#pragma once

#include <base/types.hpp>
#include <cpu/types.hpp>
#include <expected>
#include <libk/inplace_vector.hpp>
#include <optional>
#include <mm/table.hpp>

// Linker-produced facts, independent of board resources and runtime owners.
// All spans are page-aligned; the kernel VA/PA relation is affine.
struct BootSpan {
    usize pa, size;
    auto pages() const noexcept -> mm::Pages {
        return {mm::Page{pa / mm::page_size}, size / mm::page_size};
    }
};
struct BootLayout {
    usize va;
    BootSpan kernel, entry, secondary, scratch;
    auto phys(mm::Virt address) const noexcept -> std::optional<mm::Phys> {
        return address.raw() >= va && address.raw() - va < kernel.size
            ? std::optional{mm::Phys{kernel.pa + address.raw() - va}} : std::nullopt;
    }
};
static_assert(sizeof(BootSpan) == 2 * sizeof(usize) && sizeof(BootLayout) == 9 * sizeof(usize));
extern "C" const BootLayout boot_layout;

struct BootModule final {
    mm::Phys physical{};
    usize size{};
    mm::Pages pages{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return size != 0 && pages.valid();
    }
};

struct BootInfo final {
    BootModule firmware{};
    std::optional<BootModule> module{};
    libk::InplaceVector<CpuHwId, MaxCpus> cpus{};
    u64 timebase_frequency{};
};

// Boot assembles permanent mappings before exposing the hardware root.
auto kernel_root(mm::Pmm&) noexcept -> std::expected<mm::PageTable, mm::PtErr>;
