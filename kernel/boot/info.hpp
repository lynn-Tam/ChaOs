#pragma once

#include <libk/types.hpp>
#include <cpu/types.hpp>
#include <expected>
#include <libk/inplace_vector.hpp>
#include <optional>
#include <mm/table.hpp>
#include <io/host.hpp>
#include <time.hpp>
#include <string_view>
#include <variant>

// Linker-produced facts, independent of board resources and runtime owners.
// Linker spans are page-aligned; firmware spans may have partial edge pages.
// The kernel VA/PA relation is affine.
struct BootSpan {
    usize pa{}, size{};
    explicit operator bool() const noexcept { return pages().valid(); }
    auto pages() const noexcept -> mm::Pages {
        return mm::Pages::covering_bytes(mm::pa_t{pa}, size).value_or(mm::Pages{});
    }
};
struct BootLayout {
    usize va;
    BootSpan kernel, entry, secondary, scratch;
    auto phys(mm::va_t address) const noexcept -> std::optional<mm::pa_t> {
        return address.raw() >= va && address.raw() - va < kernel.size
            ? std::optional{mm::pa_t{kernel.pa + address.raw() - va}} : std::nullopt;
    }
};
static_assert(sizeof(BootSpan) == 2 * sizeof(usize) && sizeof(BootLayout) == 9 * sizeof(usize));
extern "C" const BootLayout boot_layout;

struct BootInfo final {
    BootSpan firmware{};
    u32 fw_format{};
    std::optional<BootSpan> module{};
    libk::inplace_vector<CpuHwId, MaxCpus> cpus{};
    u64 timebase_frequency{};
};

// Boot assembles permanent mappings before exposing the hardware root.
auto kernel_root(mm::Pmm&) noexcept -> std::expected<mm::PageTable, mm::PtErr>;

// Composition belongs to the firmware entry. Resource emission is synchronous;
// hardware backends and the returned interrupt entry live for the machine lifetime.
using HwRes = std::variant<io::Reg, irq::Line, io::Bus*>;
using HwEmit = libk::delegate<bool(u32 node, u32 index, HwRes) noexcept>;
using ExtIrq = libk::delegate<void() noexcept>;
using BootRead = bool (*)(BootInfo&, mm::RegionList&, CpuHwId, mm::pa_t) noexcept;
using HwStart = ExtIrq (*)(mm::Pmm&, const time::Clock&, HwEmit) noexcept;
[[noreturn]] void boot_start(CpuHwId, mm::pa_t, BootRead, HwStart) noexcept;
