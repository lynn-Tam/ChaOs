#include <start.hpp>
#pragma once

#include <libk/fmt.hpp>
#include <stddef.h>
#include <stdint.h>

// A mapped NS16550 port.  The service owns the mapping and the IRQ
// capability; this class only performs accesses within that mapping.
class uart_port final {
public:
    explicit uart_port(uintptr_t mapped_base) noexcept
        : base_(reinterpret_cast<volatile uint8_t*>(mapped_base)) {}

    [[nodiscard]] auto valid() const noexcept -> bool {
        return base_ != nullptr;
    }

    /* Destructive device setup belongs to the durable mapping owner.  It
     * resets both FIFOs and leaves receive interrupts disabled so a worker
     * generation can attach without discarding an in-flight byte. */
    void reset(uint16_t divisor = 1) const noexcept {
        if (!valid()) {
            return;
        }
        reg(Ier) = 0;
        reg(Lcr) = LcrDlab;
        reg(Rbr) = static_cast<uint8_t>(divisor & 0xff);
        reg(Ier) = static_cast<uint8_t>(divisor >> 8);
        reg(Lcr) = Lcr8n1;
        reg(Fcr) = FcrReset;
        reg(Ier) = 0;
    }

    /* Worker generations only enable RX.  Preserve any unrelated interrupt
     * enables configured by the durable device owner. */
    void enable_rx() const noexcept {
        if (!valid()) {
            return;
        }
        reg(Ier) = static_cast<uint8_t>(reg(Ier) | IerRx);
    }

    [[nodiscard]] auto tx_ready() const noexcept -> bool {
        return valid() && (reg(Lsr) & LsrTxEmpty) != 0;
    }

    [[nodiscard]] auto rx_ready() const noexcept -> bool {
        return valid() && (reg(Lsr) & LsrData) != 0;
    }

    [[nodiscard]] auto get() const noexcept -> uint8_t {
        return valid() ? reg(Rbr) : 0;
    }

    [[nodiscard]] auto try_get(uint8_t& value) const noexcept -> bool {
        if (!rx_ready()) {
            return false;
        }
        value = reg(Rbr);
        return true;
    }

    void put(char value) const noexcept {
        if (!valid()) {
            return;
        }
        while (!tx_ready()) {
        }
        reg(Thr) = static_cast<uint8_t>(value);
    }

    void write(const char* text) const noexcept {
        if (text == nullptr) {
            return;
        }
        while (*text != '\0') {
            put(*text++);
        }
    }

    void write(const char* text, size_t size) const noexcept {
        if (text == nullptr) {
            return;
        }
        for (size_t index = 0; index < size; ++index) {
            put(text[index]);
        }
    }

private:
    static constexpr size_t Rbr = 0;
    static constexpr size_t Thr = 0;
    static constexpr size_t Ier = 1;
    static constexpr size_t Fcr = 2;
    static constexpr size_t Lcr = 3;
    static constexpr size_t Lsr = 5;
    static constexpr uint8_t LsrData = 1u << 0;
    static constexpr uint8_t LsrTxEmpty = 1u << 5;
    static constexpr uint8_t LcrDlab = 1u << 7;
    static constexpr uint8_t Lcr8n1 = 0x03;
    static constexpr uint8_t FcrReset = 0x07;
    static constexpr uint8_t IerRx = 0x01;

    [[nodiscard]] auto reg(size_t offset) const noexcept -> volatile uint8_t& {
        return base_[offset];
    }

    volatile uint8_t* base_{};
};

