#pragma once

#include <base/types.hpp>
#include <mm/types.hpp>
#include <optional>

namespace arch {
inline constexpr int PtLevels = 3, PtBits = 9;
inline constexpr usize Ptes = usize{1} << PtBits;
inline constexpr usize PtUserEnd = usize{1} << (12 + PtBits * PtLevels - 1);
inline constexpr usize PtKernelBegin = ~(PtUserEnd - 1);
constexpr bool pt_canonical(usize addr) noexcept {
    return addr < PtUserEnd || addr >= PtKernelBegin;
}
auto pt_token(mm::Page) noexcept -> usize;
void activate_root(usize satp) noexcept;
bool root_active(usize satp) noexcept;
void flush_tlb_all() noexcept;
enum class PtPerm : u8 { Ro = 1, Rw = 3, Rx = 5, UserRo = 9, UserRw = 11, UserRx = 13, UserX = 12 };

// Sv39 encoding only. Tree ownership and policy belong to PageTable.
class Pte {
public:
    constexpr Pte() noexcept = default;
    static constexpr auto from_raw(u64 bits) noexcept -> Pte { return Pte{bits}; }
    static constexpr auto non_leaf(mm::Page page) noexcept -> std::optional<Pte> {
        auto ppn = encode(page);
        return ppn ? std::optional{Pte{*ppn | Valid}} : std::nullopt;
    }
    // PBMT=0 inherits PMA. Explicit overrides need Svpbmt and firmware enable;
    // neither is negotiated yet, so reject them instead of silently dropping them.
    static constexpr bool supports(mm::CpuAttr attr) noexcept { return attr == mm::CpuAttr::Native; }
    static constexpr auto leaf_4k(mm::Page page, PtPerm perms, bool warm = true,
                                  mm::CpuAttr attr = mm::CpuAttr::Native) noexcept
        -> std::optional<Pte> {
        auto ppn = encode(page);
        return ppn && supports(attr) ? std::optional{Pte{*ppn | (u64(perms) << 1) | Valid | (warm ? Used : 0)}}
                   : std::nullopt;
    }
    auto load() const noexcept -> Pte {
        Pte value;
        __atomic_load(this, &value, __ATOMIC_ACQUIRE);
        return value;
    }
    void store(Pte value) noexcept { __atomic_store(this, &value, __ATOMIC_RELEASE); }
    auto exchange(Pte value) noexcept -> Pte {
        Pte old;
        __atomic_exchange(this, &value, &old, __ATOMIC_ACQ_REL);
        return old;
    }
    // Software serializes mutations; CAS folds in concurrent hardware A/D writes.
    template <class F> auto update(F f) noexcept -> Pte {
        auto old = load();
        for (;;) {
            auto next = f(old);
            if (__atomic_compare_exchange(this, &old, &next, false, __ATOMIC_ACQ_REL,
                                          __ATOMIC_ACQUIRE))
                return old;
        }
    }
    constexpr auto raw() const noexcept -> u64 { return bits_; }
    constexpr bool accessed() const noexcept { return bits_ & Accessed; }
    constexpr bool dirty() const noexcept { return bits_ & Dirty; }
    constexpr bool valid() const noexcept { return bits_ & Valid; }
    constexpr bool is_leaf() const noexcept {
        const auto rwx = bits_ & Rwx;
        return valid() && rwx && (rwx & 6) != 4;
    }
    constexpr bool is_non_leaf() const noexcept { return valid() && !(bits_ & Rwx); }
    constexpr auto next_table_page() const noexcept -> std::optional<mm::Page> {
        return is_non_leaf() && !(bits_ & ~(Ppn | Valid | Global | Rsw))
                   ? std::optional{mm::Page{(bits_ & Ppn) >> 10}}
                   : std::nullopt;
    }
    constexpr auto leaf_page() const noexcept -> std::optional<mm::Page> {
        return is_leaf() && !(bits_ & ~(Ppn | Valid | Perm | Global | Used | Rsw))
                   ? std::optional{mm::Page{(bits_ & Ppn) >> 10}}
                   : std::nullopt;
    }
    constexpr auto perms() const noexcept -> std::optional<PtPerm> {
        return leaf_page() ? std::optional{PtPerm((bits_ & Perm) >> 1)} : std::nullopt;
    }
    constexpr bool has_permissions(PtPerm perms) const noexcept {
        return leaf_page() && (bits_ & Perm) == (u64(perms) << 1);
    }
    constexpr auto with_permissions(PtPerm perms) const noexcept -> Pte {
        return Pte{(bits_ & ~Perm) | (u64(perms) << 1)};
    }
    constexpr auto with_usage(bool accessed, bool dirty) const noexcept -> Pte {
        return Pte{(bits_ & ~Used) | (accessed ? Accessed : 0) | (dirty ? Dirty : 0)};
    }

private:
    explicit constexpr Pte(u64 bits) noexcept : bits_(bits) {}
    static constexpr auto encode(mm::Page page) noexcept -> std::optional<u64> {
        return page.valid() && page.raw() <= ((u64{1} << 44) - 1)
                   ? std::optional{u64(page.raw()) << 10}
                   : std::nullopt;
    }
    static constexpr u64 Valid = 1, Rwx = 14, Perm = 30, Global = 32, Accessed = 64, Dirty = 128,
                         Used = 192, Rsw = 768, Ppn = ((u64{1} << 44) - 1) << 10;
    u64 bits_{};
};
static_assert(sizeof(Pte) * Ptes == mm::page_size);
} // namespace arch
