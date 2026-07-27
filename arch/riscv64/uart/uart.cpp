#include <arch/uart.hpp>

namespace arch::riscv64 {
namespace {
constexpr usize Rbr = 0;
constexpr usize Thr = 0;
constexpr usize Ier = 1;
constexpr usize Fcr = 2;
constexpr usize Lcr = 3;
constexpr usize Lsr = 5;
constexpr u8 LsrData = 1 << 0;
constexpr u8 LsrEmpty = 1 << 5;
constexpr u8 LcrDlab = 1 << 7;
constexpr usize PlicPriority = 0x0000;
constexpr usize PlicEnableS = 0x2080;
constexpr usize PlicThresholdS = 0x201000;
constexpr usize PlicClaimS = 0x201004;
}

void Uart16550::initialize(u16 divisor) noexcept {
    *reg(Ier) = 0;
    *reg(Lcr) = LcrDlab;
    *reg(0) = static_cast<u8>(divisor & 0xff);
    *reg(1) = static_cast<u8>(divisor >> 8);
    *reg(Lcr) = 0x03; // 8N1
    *reg(Fcr) = 0x07; // enable and clear FIFOs
    *reg(Ier) = 0x01; // receive-ready interrupt
}

auto Uart16550::ready() const noexcept -> bool {
    return (*reg(Lsr) & LsrEmpty) != 0;
}

auto Uart16550::rx_ready() const noexcept -> bool {
    return (*reg(Lsr) & LsrData) != 0;
}

auto Uart16550::read() const noexcept -> u8 {
    return *reg(Rbr);
}

void Uart16550::write(u8 value) const noexcept {
    while (!ready()) {
    }
    *reg(Thr) = value;
}

void Uart16550::write(const char* text) const noexcept {
    if (text == nullptr) {
        return;
    }
    while (*text != '\0') {
        write(static_cast<u8>(*text++));
    }
}

void Plic::initialize(u32 source, u32 priority) const noexcept {
    if (source == 0 || source >= 32) {
        return;
    }
    word(PlicPriority + source * sizeof(u32))[0] = priority;
    word(PlicThresholdS)[0] = 0;
    unmask(source);
}

void Plic::mask(u32 source) const noexcept {
    if (source == 0 || source >= 32) {
        return;
    }
    *word(PlicEnableS) &= ~(u32{1} << source);
}

void Plic::unmask(u32 source) const noexcept {
    if (source == 0 || source >= 32) {
        return;
    }
    *word(PlicEnableS) |= u32{1} << source;
}

auto Plic::claim() const noexcept -> u32 {
    return *word(PlicClaimS);
}

void Plic::complete(u32 source) const noexcept {
    *word(PlicClaimS) = source;
}

} // namespace arch::riscv64
