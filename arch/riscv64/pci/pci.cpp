#include <arch/pci.hpp>

#include <libk/utility.hpp>
#include <mm/virtual_layout.hpp>

namespace arch {
namespace {
constexpr usize Alias = kernel::mm::layout::DirectMapBegin;
constexpr u16 Command = 4;
constexpr u16 MemoryDecode = 2;
constexpr u16 BusMaster = 4;
constexpr u16 InterruptDisable = 1 << 10;
constexpr usize WindowSize = 0x10'0000;

template<typename T>
auto read(usize address) noexcept -> T {
    asm volatile("fence iorw, iorw" ::: "memory");
    const T value = *reinterpret_cast<volatile const T*>(address);
    asm volatile("fence iorw, iorw" ::: "memory");
    return value;
}
template<typename T>
void write(usize address, T value) noexcept {
    asm volatile("fence iorw, iorw" ::: "memory");
    *reinterpret_cast<volatile T*>(address) = value;
    asm volatile("fence iorw, iorw" ::: "memory");
}
auto config(u16 requester) noexcept -> usize {
    return Alias + virt_pci_ecam + (usize{requester} << 12);
}
} // namespace

auto PciFunction::discover_block() noexcept
    -> libk::Expected<PciFunction, PciError> {
    PciFunction function{};
    bool found{};
    for (u16 slot = 0; slot < 32; ++slot) {
        const u16 requester = static_cast<u16>(slot << 3);
        const usize candidate = config(requester);
        if (read<u32>(candidate) != 0x1042'1af4) continue;
        if (found || read<u8>(candidate + 14) != 0)
            return libk::unexpected(PciError::Unsupported);
        found = true;
        function.requester_ = requester;
        function.config_ = candidate;
    }
    if (!found) return libk::unexpected(PciError::Absent);

    const u8 pin = read<u8>(function.config_ + 0x3d);
    if (pin == 0 || pin > 4) return libk::unexpected(PciError::Unsupported);
    // QEMU virt root-bus interrupt-map swizzles INTA..INTD by slot.
    function.irq_source_ = virt_pci_irq_first
        + ((function.requester_ >> 3) + pin - 1) % virt_pci_irq_count;

    function.disable_dma();
    write<u16>(function.config_ + Command, InterruptDisable);
    usize next = virt_pci_memory;
    for (usize index = 0; index < function.bars_.size(); ++index) {
        const usize reg = function.config_ + 0x10 + 4 * index;
        const u32 original = read<u32>(reg);
        if ((original & 1) != 0) return libk::unexpected(PciError::Unsupported);
        const bool wide = (original & 6) == 4;
        if ((original & 6) != 0 && !wide)
            return libk::unexpected(PciError::Unsupported);
        if (wide && index == 5) return libk::unexpected(PciError::Invalid);
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
            return libk::unexpected(PciError::Unsupported);
        // A user BAR mapping covers whole CPU pages. Reserve that full span
        // so a small BAR cannot expose an adjacent register bank.
        const usize allocation = extent < 4096 ? 4096 : extent;
        const usize aligned = (next + allocation - 1) & ~(allocation - 1);
        if (aligned > virt_pci_memory + WindowSize - allocation)
            return libk::unexpected(PciError::Unsupported);
        function.bars_[index] = PciBar{
            .address = aligned, .size = static_cast<usize>(extent),
            .attributes = original & 15};
        write<u32>(reg, static_cast<u32>(aligned) | (original & 15));
        if (wide) {
            write<u32>(reg + 4, 0);
            ++index;
        }
        next = aligned + allocation;
    }

    if ((read<u16>(function.config_ + 6) & 0x10) == 0)
        return libk::unexpected(PciError::Unsupported);
    u8 capability = read<u8>(function.config_ + 0x34);
    usize status{};
    for (usize count = 0; capability != 0 && count < 48; ++count) {
        if (capability < 0x40 || (capability & 3) != 0)
            return libk::unexpected(PciError::Invalid);
        const usize cap = function.config_ + capability;
        const u8 id = read<u8>(cap);
        if (id == 9 && read<u8>(cap + 3) == 1) {
            if (capability > 0xf0 || read<u8>(cap + 2) < 16 || status != 0)
                return libk::unexpected(PciError::Invalid);
            const u8 bar = read<u8>(cap + 4);
            const u32 offset = read<u32>(cap + 8);
            const u32 length = read<u32>(cap + 12);
            if (bar >= function.bars_.size() || length < 21
                || offset > function.bars_[bar].size
                || length > function.bars_[bar].size - offset)
                return libk::unexpected(PciError::Invalid);
            status = Alias + function.bars_[bar].address + offset + 20;
        }
        capability = read<u8>(cap + 1);
    }
    if (capability != 0 || status == 0)
        return libk::unexpected(PciError::Unsupported);
    function.status_ = status;
    write<u16>(function.config_ + Command, MemoryDecode | InterruptDisable);
    // Immutable discovery metadata for userspace. BAR address authority is
    // exported separately through bounded MemoryObjects.
    for (usize index = 0; index < function.configuration_.size(); ++index)
        function.configuration_[index] = read<u32>(function.config_ + index * 4);
    for (usize index = 0; index < function.bars_.size(); ++index)
        function.configuration_[4 + index] = function.bars_[index].attributes;
    return libk::expected(libk::move(function));
}

auto PciFunction::config32(u16 offset) const noexcept -> u32 {
    return read<u32>(config_ + (offset & 0xffc));
}
void PciFunction::enable_dma() const noexcept {
    write<u16>(config_ + Command, MemoryDecode | BusMaster);
}
void PciFunction::disable_dma() const noexcept {
    write<u16>(config_ + Command,
        static_cast<u16>((read<u16>(config_ + Command) & ~BusMaster) | InterruptDisable));
}
void PciFunction::flush_mmio() const noexcept {
    static_cast<void>(read<u8>(status_));
}
void PciFunction::begin_reset() const noexcept {
    write<u8>(status_, 0);
}
auto PciFunction::reset_complete() const noexcept -> bool {
    return read<u8>(status_) == 0;
}

} // namespace arch
