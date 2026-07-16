#pragma once

#include <core/types.hpp>
#include <cpu/topology.hpp>

namespace kernel {

// Stage D deliberately bounds the logical CPU namespace.  Firmware IDs may
// remain sparse and arbitrarily large; only the dense logical IDs published by
// CpuRegistry enter scheduler and translation state.
inline constexpr usize max_cpu_count = 256;

class CpuSet final {
public:
    static constexpr usize word_bits = sizeof(u64) * 8;
    static constexpr usize word_count = max_cpu_count / word_bits;

    [[nodiscard]] constexpr auto empty() const noexcept -> bool {
        for (const u64 word : words_) {
            if (word != 0) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] constexpr auto contains(CpuId id) const noexcept -> bool {
        return id.raw < max_cpu_count
            && (words_[id.raw / word_bits]
                & (u64{1} << (id.raw % word_bits))) != 0;
    }

    [[nodiscard]] constexpr auto insert(CpuId id) noexcept -> bool {
        if (id.raw >= max_cpu_count) {
            return false;
        }
        u64& word = words_[id.raw / word_bits];
        const u64 bit = u64{1} << (id.raw % word_bits);
        const bool inserted = (word & bit) == 0;
        word |= bit;
        return inserted;
    }

    [[nodiscard]] constexpr auto erase(CpuId id) noexcept -> bool {
        if (id.raw >= max_cpu_count) {
            return false;
        }
        u64& word = words_[id.raw / word_bits];
        const u64 bit = u64{1} << (id.raw % word_bits);
        const bool erased = (word & bit) != 0;
        word &= ~bit;
        return erased;
    }

    [[nodiscard]] auto size() const noexcept -> usize {
        usize result{};
        for (u64 word : words_) {
            while (word != 0) {
                word &= word - 1;
                ++result;
            }
        }
        return result;
    }

    template<typename Function>
    constexpr void for_each(Function&& function) const noexcept {
        for (usize word_index = 0; word_index < word_count; ++word_index) {
            u64 pending = words_[word_index];
            for (usize bit = 0; pending != 0; ++bit, pending >>= 1) {
                if ((pending & 1) != 0) {
                    function(CpuId{word_index * word_bits + bit});
                }
            }
        }
    }

    [[nodiscard]] friend constexpr auto operator==(
        const CpuSet&, const CpuSet&) noexcept -> bool = default;

private:
    static_assert(max_cpu_count % word_bits == 0);
    u64 words_[word_count]{};
};

static_assert(sizeof(CpuSet) == max_cpu_count / 8);

} // namespace kernel
