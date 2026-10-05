#pragma once

#include <base/types.hpp>

namespace arch::riscv64 {

class Uart16550 final {
public:
    explicit Uart16550(usize base) noexcept
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

} // namespace arch::riscv64
