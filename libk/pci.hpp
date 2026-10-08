#pragma once
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace pci {
struct Bar { uint64_t pa{}, size{}; bool wide{}; };

// Configuration protocol only. The caller owns the function and provides
// the CPU's I/O ordering primitive; no allocation or board addresses live here.
template<auto fence> struct Cfg {
    uintptr_t base;
    template<class T> auto read(size_t offset) const noexcept -> T {
        fence();
        const T value = *reinterpret_cast<volatile const T*>(base + offset);
        fence();
        return value;
    }
    template<class T> void write(size_t offset, T value) const noexcept {
        fence();
        *reinterpret_cast<volatile T*>(base + offset) = value;
        fence();
    }
    // Decode and DMA must be off while probing. Restore every BAR and the
    // original command before returning, including malformed configurations.
    auto bars() const noexcept -> std::optional<std::array<Bar, 6>> {
        std::array<Bar, 6> result{};
        const auto command = read<uint16_t>(4);
        write<uint16_t>(4, (command & ~uint16_t{7}) | uint16_t{1 << 10});
        bool valid = true;
        for (size_t i = 0; i < result.size(); ++i) {
            const size_t at = 0x10 + 4 * i;
            const auto low = read<uint32_t>(at);
            const bool wide = (low & 6) == 4;
            if ((low & 1) || ((low & 6) && !wide) || (wide && i == 5)) {
                valid = false; break;
            }
            const auto high = wide ? read<uint32_t>(at + 4) : 0;
            write<uint32_t>(at, UINT32_MAX);
            if (wide) write<uint32_t>(at + 4, UINT32_MAX);
            const auto mask = read<uint32_t>(at) & ~uint32_t{15};
            const auto upper = wide ? read<uint32_t>(at + 4) : UINT32_MAX;
            write<uint32_t>(at, low);
            if (wide) write<uint32_t>(at + 4, high);
            if (!mask && !wide) continue;
            const uint64_t size = ~((uint64_t{upper} << 32) | mask) + 1;
            if (!std::has_single_bit(size)) { valid = false; break; }
            result[i] = {(uint64_t{high} << 32) | (low & ~uint32_t{15}), size, wide};
            if (wide) ++i;
        }
        write<uint16_t>(4, command);
        return valid ? std::optional{result} : std::nullopt;
    }
};
} // namespace pci
