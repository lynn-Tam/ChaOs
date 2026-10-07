#include <cpu.hpp>
#include <expected>
#include "pci.hpp"
#include "config.hpp"

#include <utility>
#include <mm/table.hpp>

namespace {
constexpr usize Alias = mm::DirectBegin;
constexpr u16 Command = 4;
constexpr u16 MemoryDecode = 2;
constexpr u16 BusMaster = 4;
constexpr u16 InterruptDisable = 1 << 10;
constexpr usize WindowSize = 0x10'0000;

template<typename T>
auto read(usize address) noexcept -> T {
    arch::io_fence();
    const T value = *reinterpret_cast<volatile const T*>(address);
    arch::io_fence();
    return value;
}
template<typename T>
void write(usize address, T value) noexcept {
    arch::io_fence();
    *reinterpret_cast<volatile T*>(address) = value;
    arch::io_fence();
}
auto config(u16 requester) noexcept -> usize {
    return Alias + VirtPciEcam + (usize{requester} << 12);
}
} // namespace

auto PciFn::scan(Found found) noexcept
    -> std::expected<void, PciError> {
    bool any = false;
    usize next = VirtPciMmio;
    for (u16 slot = 0; slot < 32; ++slot) {
        const usize primary = config(static_cast<u16>(slot << 3));
        if (read<u16>(primary) == 0xffff) continue;
        const u8 functions = (read<u8>(primary + 14) & 0x80) != 0 ? 8 : 1;
        for (u16 number = 0; number < functions; ++number) {
            const u16 requester = static_cast<u16>((slot << 3) | number);
            const usize candidate = config(requester);
            if (read<u16>(candidate) == 0xffff) continue;
            // Unknown root-bus functions remain quiescent and default-denied
            // by the IOMMU until a driver and reset protocol are supported.
            write<u16>(candidate + Command, InterruptDisable);
            if (read<u32>(candidate) != 0x1042'1af4) continue;
            if ((read<u8>(candidate + 14) & 0x7f) != 0)
                return std::unexpected(PciError::Unsupported);
            PciFn function{};
            function.requester_ = requester;
            function.config_ = candidate;
            const u8 pin = read<u8>(candidate + 0x3d);
            if (pin == 0 || pin > 4)
                return std::unexpected(PciError::Unsupported);
            // QEMU virt root-bus interrupt-map swizzles INTA..INTD by slot.
            function.irq_source_ = VirtPciIrq
                + (slot + pin - 1) % VirtPciPins;
            auto configured = function.configure_bars(next);
            if (!configured) return configured;
            any = true;
            if (!found(std::move(function)))
                return std::unexpected(PciError::Unsupported);
        }
    }
    return !any ? std::expected<void, PciError>{std::unexpected(PciError::Absent)}
                          : std::expected<void, PciError>{};
}

auto PciFn::configure_bars(usize& next) noexcept
    -> std::expected<void, PciError> {
    for (usize index = 0; index < bars_.size(); ++index) {
        const usize reg = config_ + 0x10 + 4 * index;
        const u32 original = read<u32>(reg);
        if ((original & 1) != 0) return std::unexpected(PciError::Unsupported);
        const bool wide = (original & 6) == 4;
        if ((original & 6) != 0 && !wide)
            return std::unexpected(PciError::Unsupported);
        if (wide && index == 5) return std::unexpected(PciError::Invalid);
        const u32 upper = wide ? read<u32>(reg + 4) : 0;
        write<u32>(reg, 0xffff'ffff);
        if (wide) write<u32>(reg + 4, 0xffff'ffff);
        const u32 mask = read<u32>(reg) & ~u32{15};
        const u32 high_mask = wide ? read<u32>(reg + 4) : 0xffff'ffff;
        write<u32>(reg, original);
        if (wide) write<u32>(reg + 4, upper);
        if (mask == 0 && !wide) continue;
        const u64 extent = ~((u64{high_mask} << 32) | mask) + 1;
        if (extent == 0 || extent > WindowSize
            || (extent & (extent - 1)) != 0)
            return std::unexpected(PciError::Unsupported);
        // A user BAR mapping covers whole CPU pages. Reserve that full span
        // so a small BAR cannot expose an adjacent register bank.
        const usize allocation = extent < 4096 ? 4096 : extent;
        const usize aligned = (next + allocation - 1) & ~(allocation - 1);
        if (aligned > VirtPciMmio + WindowSize - allocation)
            return std::unexpected(PciError::Unsupported);
        bars_[index] = io::Bar{
            .address = aligned, .size = static_cast<usize>(extent),
            .attributes = original & 15};
        write<u32>(reg, static_cast<u32>(aligned) | (original & 15));
        if (wide) {
            write<u32>(reg + 4, 0);
            ++index;
        }
        next = aligned + allocation;
    }

    if ((read<u16>(config_ + 6) & 0x10) == 0)
        return std::unexpected(PciError::Unsupported);
    u8 capability = read<u8>(config_ + 0x34);
    usize status{};
    for (usize count = 0; capability != 0 && count < 48; ++count) {
        if (capability < 0x40 || (capability & 3) != 0)
            return std::unexpected(PciError::Invalid);
        const usize cap = config_ + capability;
        const u8 id = read<u8>(cap);
        if (id == 9 && read<u8>(cap + 3) == 1) {
            if (capability > 0xf0 || read<u8>(cap + 2) < 16 || status != 0)
                return std::unexpected(PciError::Invalid);
            const u8 bar = read<u8>(cap + 4);
            const u32 offset = read<u32>(cap + 8);
            const u32 length = read<u32>(cap + 12);
            if (bar >= bars_.size() || length < 21
                || offset > bars_[bar].size
                || length > bars_[bar].size - offset)
                return std::unexpected(PciError::Invalid);
            status = Alias + bars_[bar].address + offset + 20;
        }
        capability = read<u8>(cap + 1);
    }
    if (capability != 0 || status == 0)
        return std::unexpected(PciError::Unsupported);
    status_ = status;
    write<u16>(config_ + Command, MemoryDecode | InterruptDisable);
    // Immutable discovery metadata for userspace. BAR address authority is
    // exported separately through bounded MemoryObjects.
    for (usize index = 0; index < configuration_.size(); ++index)
        configuration_[index] = read<u32>(config_ + index * 4);
    for (usize index = 0; index < bars_.size(); ++index)
        configuration_[4 + index] = bars_[index].attributes;
    return {};
}

auto PciFn::config32(u16 offset) const noexcept -> u32 {
    return read<u32>(config_ + (offset & 0xffc));
}
void PciFn::enable_dma() const noexcept {
    write<u16>(config_ + Command, MemoryDecode | BusMaster);
}
void PciFn::disable_dma() const noexcept {
    write<u16>(config_ + Command,
        static_cast<u16>((read<u16>(config_ + Command) & ~BusMaster) | InterruptDisable));
}
void PciFn::flush_mmio() const noexcept {
    static_cast<void>(read<u8>(status_));
}
void PciFn::begin_reset() const noexcept {
    write<u8>(status_, 0);
}
auto PciFn::reset_complete() const noexcept -> bool {
    return read<u8>(status_) == 0;
}
