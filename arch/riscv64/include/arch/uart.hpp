#pragma once

#include <core/types.hpp>

namespace arch::riscv64 {

// QEMU virt exposes a NS16550-compatible UART at this address.  The driver
// only performs MMIO after the caller has installed a Device MemoryObject
// mapping; the constant is not a userspace physical-frame authority.
inline constexpr usize virt_uart_base = 0x1000'0000;
inline constexpr u32 virt_uart_irq = 10;
inline constexpr usize virt_plic_base = 0x0c00'0000;
inline constexpr usize virt_plic_size = 0x0040'0000;

class Uart16550 final {
public:
    explicit Uart16550(usize base = virt_uart_base) noexcept
        : base_(base) {}

    void initialize(u16 divisor = 1) noexcept;
    [[nodiscard]] auto ready() const noexcept -> bool;
    [[nodiscard]] auto rx_ready() const noexcept -> bool;
    [[nodiscard]] auto read() const noexcept -> u8;
    void write(u8 value) const noexcept;
    void write(const char* text) const noexcept;

private:
    [[nodiscard]] volatile u8* reg(usize offset) const noexcept {
        return reinterpret_cast<volatile u8*>(base_ + offset);
    }

    usize base_{};
};

class Plic final {
public:
    explicit constexpr Plic(usize base = virt_plic_base) noexcept
        : base_(base) {}

    void initialize(u32 source, u32 priority = 1) const noexcept;
    void mask(u32 source) const noexcept;
    void unmask(u32 source) const noexcept;
    [[nodiscard]] auto claim() const noexcept -> u32;
    void complete(u32 source) const noexcept;

private:
    [[nodiscard]] volatile u32* word(usize offset) const noexcept {
        return reinterpret_cast<volatile u32*>(base_ + offset);
    }

    usize base_{};
};

} // namespace arch::riscv64
