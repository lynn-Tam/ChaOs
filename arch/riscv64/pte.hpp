#pragma once

#include <libk/types.hpp>
#include <optional>
#include <uapi/riscv64.h>

namespace arch {
inline constexpr int PtLevels = 3, PtBits = 9;
inline constexpr usize Ptes = usize{1} << PtBits;
inline constexpr usize PtUserBegin = RISCV64_LOW_GUARD_END;
inline constexpr usize PtUserEnd = usize{1} << (12 + PtBits * PtLevels - 1);
inline constexpr usize IoBegin = 4096; // Zero selects automatic IOVA placement.
// First-stage Sv39 IOVA uses the contiguous low canonical half.
inline constexpr usize IoEnd = usize{1} << (12 + PtBits * PtLevels - 1);
inline constexpr usize PtKernelBegin = ~(PtUserEnd - 1);
constexpr bool pt_canonical(usize addr) noexcept {
    return addr < PtUserEnd || addr >= PtKernelBegin;
}
auto pt_token(usize ppn) noexcept -> usize;
void load_root(usize satp) noexcept;
auto asid_mask() noexcept -> usize;
auto tagged_root(usize satp, usize asid) noexcept -> usize;
void flush_tlb(usize asid) noexcept;
void flush_tlb_page(usize address, usize asid) noexcept;
bool flush_remote(usize hart) noexcept;
bool root_active(usize satp) noexcept;
void flush_tlb_all() noexcept;
enum class PtPerm : u8 { Ro = 1, Rw = 3, Rx = 5, UserRo = 9, UserRw = 11, UserRx = 13, UserX = 12 };

// Sv39 encoding takes physical page numbers; no allocator ownership or address policy.
class Pte {
public:
    static constexpr usize PhysicalPages = usize{1} << 44;
    constexpr Pte() noexcept = default;
    static constexpr auto from_raw(u64 bits) noexcept -> Pte { return Pte{bits}; }
    static constexpr auto non_leaf(usize page) noexcept -> std::optional<Pte> {
        auto ppn = encode(page);
        return ppn ? std::optional{Pte{*ppn | Valid}} : std::nullopt;
    }
    // PBMT stays zero: CPU accesses inherit platform PMAs. No cache override API.
    static constexpr auto leaf(usize page, PtPerm perms, bool warm = true) noexcept
        -> std::optional<Pte> {
        auto ppn = encode(page);
        return ppn ? std::optional{Pte{*ppn | (u64(perms) << 1) | Valid | (warm ? Used : 0)}}
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
    constexpr auto next_table_page() const noexcept -> std::optional<usize> {
        return is_non_leaf() && !(bits_ & ~(Ppn | Valid | Global | Rsw))
                   ? std::optional{usize((bits_ & Ppn) >> 10)}
                   : std::nullopt;
    }
    constexpr auto leaf_page() const noexcept -> std::optional<usize> {
        return is_leaf() && !(bits_ & ~(Ppn | Valid | Perm | Global | Used | Rsw))
                   ? std::optional{usize((bits_ & Ppn) >> 10)}
                   : std::nullopt;
    }
    constexpr auto perms() const noexcept -> std::optional<PtPerm> {
        return leaf_page() ? std::optional{PtPerm((bits_ & Perm) >> 1)} : std::nullopt;
    }
    constexpr bool has_permissions(PtPerm perms) const noexcept {
        return leaf_page() && (bits_ & Perm) == (u64(perms) << 1);
    }
    constexpr auto with_global() const noexcept -> Pte { return Pte{bits_ | Global}; }
    constexpr auto with_permissions(PtPerm perms) const noexcept -> Pte {
        return Pte{(bits_ & ~Perm) | (u64(perms) << 1)};
    }
    constexpr auto with_usage(bool accessed, bool dirty) const noexcept -> Pte {
        return Pte{(bits_ & ~Used) | (accessed ? Accessed : 0) | (dirty ? Dirty : 0)};
    }

private:
    explicit constexpr Pte(u64 bits) noexcept : bits_(bits) {}
    static constexpr auto encode(usize page) noexcept -> std::optional<u64> {
        return page <= (PhysicalPages - 1)
                   ? std::optional{u64(page) << 10}
                   : std::nullopt;
    }
    static constexpr u64 Valid = 1, Rwx = 14, Perm = 30, Global = 32, Accessed = 64, Dirty = 128,
                         Used = 192, Rsw = 768, Ppn = (PhysicalPages - 1) << 10;
    u64 bits_{};
};
static_assert(sizeof(Pte) * Ptes == 4096);
} // namespace arch
