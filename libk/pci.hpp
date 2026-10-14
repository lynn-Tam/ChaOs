#pragma once
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

namespace pci {
struct bar { uint64_t pa{}, size{}; bool wide{}; };
struct virtio {
    std::array<std::pair<uintptr_t, size_t>, 4> regs{}; // common, notify, ISR, device
    uint32_t multiplier{};
};

// Configuration protocol only. The caller owns the function and provides
// the CPU's I/O ordering primitive; no allocation or board addresses live here.
template<auto fence> struct cfg {
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
    auto virtio(std::span<const std::pair<uintptr_t, size_t>, 6> bars) const noexcept -> std::optional<pci::virtio> {
        pci::virtio result;
        if (!(read<uint16_t>(6) & 0x10)) return {};
        uint8_t cap = read<uint8_t>(0x34);
        for (size_t count = 0; cap && count < 48; ++count) {
            if (cap < 0x40 || cap > 0xfc || cap % 4) return {};
            const auto header = read<uint32_t>(cap);
            const auto type = header >> 24;
            if ((header & 255) == 9 && type >= 1 && type <= 4) {
                if (cap > 0xf0 || ((header >> 16) & 255) < 16) return {};
                const auto bar = read<uint8_t>(cap + 4);
                const auto offset = read<uint32_t>(cap + 8), size = read<uint32_t>(cap + 12);
                if (bar >= bars.size() || offset > bars[bar].second || size > bars[bar].second - offset || result.regs[type - 1].first) return {};
                result.regs[type - 1] = {bars[bar].first + offset, size};
                if (type == 2) {
                    if (cap > 0xec || ((header >> 16) & 255) < 20) return {};
                    result.multiplier = read<uint32_t>(cap + 16);
                }
            }
            cap = header >> 8;
        }
        constexpr size_t minimum[]{56, 2, 1, 8};
        for (size_t n = 0; n < 4; ++n)
            if (!result.regs[n].first || result.regs[n].second < minimum[n]) return {};
        return cap ? std::nullopt : std::optional{result};
    }
    // Decode and DMA must be off while probing. Restore every BAR and the
    // original command before returning, including malformed configurations.
    auto bars() const noexcept -> std::optional<std::array<bar, 6>> {
        std::array<bar, 6> result{};
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
