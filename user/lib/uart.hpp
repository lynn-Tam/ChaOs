#pragma once

#include <libk/fmt.hpp>

#include <stddef.h>
#include <stdint.h>

namespace myos::uart {

// A mapped NS16550 port.  The service owns the mapping and the IRQ
// capability; this class only performs accesses within that mapping.
class Port final {
public:
    explicit Port(uintptr_t mapped_base) noexcept
        : base_(reinterpret_cast<volatile uint8_t*>(mapped_base)) {}

    [[nodiscard]] auto valid() const noexcept -> bool {
        return base_ != nullptr;
    }

    void initialize(uint16_t divisor = 1) const noexcept {
        if (!valid()) {
            return;
        }
        reg(Ier) = 0;
        reg(Lcr) = LcrDlab;
        reg(Rbr) = static_cast<uint8_t>(divisor & 0xff);
        reg(Ier) = static_cast<uint8_t>(divisor >> 8);
        reg(Lcr) = Lcr8n1;
        reg(Fcr) = FcrReset;
        reg(Ier) = IerRx;
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

// Writer is kept as the small print-facing façade used by early services.
// Port remains the owner of all UART register semantics.
class Writer final {
public:
    explicit Writer(Port port) noexcept : port_(port) {}

    [[nodiscard]] auto valid() const noexcept -> bool { return port_.valid(); }
    void put(char value) const noexcept { port_.put(value); }
    void write(const char* text) const noexcept { port_.write(text); }
    void write(const char* text, size_t size) const noexcept {
        port_.write(text, size);
    }

private:
    Port port_;
};

class Line final {
public:
    explicit Line(Writer writer) noexcept : writer_(writer) {}

    void write(const char* text) const noexcept { writer_.write(text); }
    void newline() const noexcept { writer_.put('\n'); }

private:
    Writer writer_;
};

// Fixed-format, immediate user output.  Formatting stays in libk; this sink
// only owns the UART transport and never buffers a second log stream.
class Printer final {
public:
    explicit Printer(Writer writer) noexcept : writer_(writer) {}

    void text(const char* value) noexcept { writer_.write(value); }

    template<libk::fmt::fixed_string F, typename... Args>
    [[nodiscard]] auto print(const Args&... args) noexcept -> bool {
        return libk::fmt::format_to<F>(writer_, args...).ok();
    }

private:
    Writer writer_;
};

} // namespace myos::uart
