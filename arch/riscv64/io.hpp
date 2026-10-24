#pragma once
#include <libk/types.hpp>

namespace arch {
auto plic_bytes(u32 context) noexcept -> usize;
void plic_init(usize base, u32 context, u32 sources) noexcept;
void plic_set(usize base, u32 context, u32 id, bool enable) noexcept;
auto plic_take(usize base, u32 context) noexcept -> u32;

enum class IoStep : u8 { Pending, Ready, Failed };
inline constexpr u32 IoFaults = 128;
constexpr auto iommu_levels(u32 bits) noexcept -> unsigned { return bits <= 6 ? 0 : bits <= 15 ? 1 : 2; }
constexpr auto iommu_index(u32 did, unsigned level) noexcept -> usize {
    return level ? (did >> (6 + 9 * (level - 1))) & 511 : (did & 63) * 8;
}
constexpr auto iommu_link(u64 ppn) noexcept -> u64 { return (ppn << 10) | 1; }
constexpr auto iommu_page(u64 link) noexcept -> u64 { return link >> 10; }
auto iommu_caps(usize base) noexcept -> bool;
void iommu_off(usize base) noexcept;
auto iommu_idle(usize base) noexcept -> IoStep;
auto iommu_queues(usize base, u64 commands, u64 faults) noexcept -> bool;
auto iommu_queued(usize base) noexcept -> IoStep;
void iommu_root(usize base, u64 directory, u32 bits) noexcept;
auto iommu_active(usize base, u64 directory, u32 bits) noexcept -> IoStep;
auto iommu_poll(usize base, u32 tail) noexcept -> IoStep;
void iommu_tail(usize base, u32 tail) noexcept;
auto iommu_faults(usize base) noexcept -> u32;
auto iommu_fault(const void* records, u32 head) noexcept -> u32;
void iommu_fault_head(usize base, u32 head) noexcept;
void iommu_irq_enable(usize base) noexcept;
void iommu_context(u64* record, u32 tag, u64 root, bool enable) noexcept;
void iommu_commands(u64* commands, u32& tail, u32 did) noexcept;
} // namespace arch
