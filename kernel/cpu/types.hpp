#pragma once

#include <base/types.hpp>
#include <array>
#include <bit>

// Firmware IDs remain opaque and sparse, while every normalized topology is
// bounded before it enters kernel initialization.
inline constexpr usize MaxCpus = 256;

struct CpuId final {
    usize raw{};

    friend constexpr auto operator==(CpuId lhs, CpuId rhs) noexcept -> bool {
        return lhs.raw == rhs.raw;
    }
};

// Firmware CPU identifiers are opaque and may be sparse.
struct CpuHwId final {
    usize raw{};

    friend constexpr auto operator==(
        CpuHwId lhs,
        CpuHwId rhs) noexcept -> bool {
        return lhs.raw == rhs.raw;
    }
};

enum class CpuAvail : u8 {
    Enabled,
    Disabled,
    Failed,
};

struct CpuTopo final {
    usize count{};
    usize boot_index{};
};

class CpuSet final {
public:
    static constexpr usize Bits = sizeof(u64) * 8;
    static constexpr usize Words = MaxCpus / Bits;
    [[nodiscard]] constexpr auto empty() const noexcept -> bool {
        for (auto w : words_) if (w) return false;
        return true;
    }
    [[nodiscard]] constexpr auto contains(CpuId id) const noexcept -> bool {
        return id.raw < MaxCpus && (words_[id.raw / Bits] & (u64{1} << (id.raw % Bits)));
    }
    [[nodiscard]] constexpr auto insert(CpuId id) noexcept -> bool { return change<true>(id); }
    [[nodiscard]] constexpr auto erase(CpuId id) noexcept -> bool { return change<false>(id); }
    [[nodiscard]] constexpr auto size() const noexcept -> usize {
        usize n{};
        for (auto w : words_) n += std::popcount(w);
        return n;
    }
    template<class Fn>
    constexpr void for_each(Fn&& fn) const noexcept {
        for (usize i = 0; i < words_.size(); ++i)
            for (u64 w = words_[i]; w; w &= w - 1)
                fn(CpuId{i * Bits + static_cast<usize>(std::countr_zero(w))});
    }
    friend constexpr auto operator==(const CpuSet&, const CpuSet&) noexcept -> bool = default;
private:
    template<bool Set>
    constexpr auto change(CpuId id) noexcept -> bool {
        if (id.raw >= MaxCpus) return false;
        auto& w = words_[id.raw / Bits];
        const auto bit = u64{1} << (id.raw % Bits);
        const bool old = (w & bit) != 0;
        if constexpr (Set) w |= bit;
        else w &= ~bit;
        return old != Set;
    }
    static_assert(MaxCpus % Bits == 0);
    std::array<u64, Words> words_{};
};

static_assert(sizeof(CpuSet) == MaxCpus / 8);
