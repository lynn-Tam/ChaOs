#include "io.hpp"
#include "cpu.hpp"

namespace arch {
static constexpr usize Ddtp = 0x10, Cqb = 0x18, Cqh = 0x20, Cqt = 0x24;
static constexpr usize Fqb = 0x28, Fqh = 0x30, Fqt = 0x34, Cqcsr = 0x48, Fqcsr = 0x4c, Ipsr = 0x54;
static constexpr u32 On = 1 << 16, Busy = 1 << 17, CqErrors = 7 << 8, FqErrors = 3 << 8;
template<class T> static auto read(usize base, usize offset) noexcept -> T {
    io_fence(); const auto value = *reinterpret_cast<volatile const T*>(base + offset); io_fence(); return value;
}
template<class T> static void write(usize base, usize offset, T value) noexcept {
    io_fence(); *reinterpret_cast<volatile T*>(base + offset) = value; io_fence();
}
auto iommu_caps(usize base) noexcept -> bool {
    const auto caps = read<u64>(base, 0);
    // Coherent little-endian Sv39, extended contexts, wired interrupts.
    return (caps & 0xf0) == 0x10 && (caps & (u64{1} << 9)) && (caps & (u64{1} << 22))
        && ((caps >> 28) & 3) != 0 && ((caps >> 28) & 3) != 3 && !(read<u32>(base, 8) & 5);
}
void iommu_off(usize base) noexcept {
    write<u64>(base, Ddtp, 0); write<u32>(base, Cqcsr, 0); write<u32>(base, Fqcsr, 0);
}
auto iommu_idle(usize base) noexcept -> IoStep {
    if ((read<u64>(base, Ddtp) & 16) || (read<u32>(base, Cqcsr) & (On | Busy)) || (read<u32>(base, Fqcsr) & (On | Busy))) return IoStep::Pending;
    return read<u64>(base, Ddtp) & 15 ? IoStep::Failed : IoStep::Ready;
}
auto iommu_queues(usize base, u64 commands, u64 faults) noexcept -> bool {
    write<u32>(base, 8, 2);
    if ((read<u32>(base, 8) & 7) != 2) return false;
    write<u64>(base, 0x2f8, read<u64>(base, 0x2f8) & ~u64{0xff});
    write<u64>(base, Cqb, (commands << 10) | 7); write<u64>(base, Fqb, (faults << 10) | 6);
    write<u32>(base, Cqt, 0); write<u32>(base, Fqh, 0);
    write<u32>(base, Cqcsr, 1); write<u32>(base, Fqcsr, 1); return true;
}
auto iommu_queued(usize base) noexcept -> IoStep {
    const auto cq = read<u32>(base, Cqcsr), fq = read<u32>(base, Fqcsr);
    if ((cq & CqErrors) || (fq & FqErrors)) return IoStep::Failed;
    if ((cq | fq) & Busy) return IoStep::Pending;
    return (cq & On) && (fq & On) ? IoStep::Ready : IoStep::Failed;
}
void iommu_root(usize base, u64 directory, u32 bits) noexcept {
    write<u64>(base, Ddtp, (directory << 10) | (2 + iommu_levels(bits)));
}
auto iommu_active(usize base, u64 directory, u32 bits) noexcept -> IoStep {
    if (read<u64>(base, Ddtp) & 16) return IoStep::Pending;
    return read<u64>(base, Ddtp) == ((directory << 10) | (2 + iommu_levels(bits))) ? IoStep::Ready : IoStep::Failed;
}
auto iommu_poll(usize base, u32 tail) noexcept -> IoStep {
    if (read<u32>(base, Cqcsr) & CqErrors) return IoStep::Failed;
    return read<u32>(base, Cqh) == tail ? IoStep::Ready : IoStep::Pending;
}
void iommu_tail(usize base, u32 tail) noexcept { write<u32>(base, Cqt, tail); }
auto iommu_faults(usize base) noexcept -> u32 {
    write<u32>(base, Ipsr, 3); const auto tail = read<u32>(base, Fqt);
    return tail < IoFaults && !(read<u32>(base, Fqcsr) & FqErrors) ? tail : UINT32_MAX;
}
auto iommu_fault(const void* records, u32 head) noexcept -> u32 {
    return static_cast<u32>(static_cast<volatile const u64*>(records)[head * 4] >> 40);
}
void iommu_fault_head(usize base, u32 head) noexcept { write<u32>(base, Fqh, head); }
void iommu_irq_enable(usize base) noexcept { write<u32>(base, Ipsr, 3); write<u32>(base, Fqcsr, 3); }
void iommu_context(u64* dc, u32 tag, u64 root, bool enable) noexcept {
    __atomic_store_n(&dc[0], u64{0}, __ATOMIC_RELEASE); dc[1] = 0;
    dc[2] = u64{tag} << 12; dc[3] = enable ? (u64{8} << 60) | root : 0;
    if (enable) __atomic_store_n(&dc[0], u64{1}, __ATOMIC_RELEASE);
}
void iommu_commands(u64* commands, u32& tail, u32 did) noexcept {
    const u64 batch[]{u64{3} | (u64{1} << 33) | (u64{did} << 40), u64{1}, u64{2} | (u64{1} << 12) | (u64{1} << 13)};
    for (const auto command : batch) { commands[tail * 2] = command; commands[tail * 2 + 1] = 0; tail = (tail + 1) & 255; }
}

static auto word(usize base, usize offset) noexcept -> volatile u32* { return reinterpret_cast<volatile u32*>(base + offset); }
auto plic_bytes(u32 context) noexcept -> usize { return (0x200008 + context * 0x1000 + 4095) & ~usize{4095}; }
void plic_init(usize base, u32 context, u32 sources) noexcept {
    for (usize i = 0; i <= sources / 32; ++i) *word(base, 0x2000 + context * 0x80 + i * 4) = 0;
    *word(base, 0x200000 + context * 0x1000) = 0; io_fence();
}
void plic_set(usize base, u32 context, u32 id, bool enable) noexcept {
    io_fence();
    auto* priority = word(base, id * 4); if (!*priority) *priority = 1;
    auto* bank = word(base, 0x2000 + context * 0x80 + (id / 32) * 4);
    if (enable) { *bank = *bank | (u32{1} << (id % 32)); *priority = *priority; }
    else *bank = *bank & ~(u32{1} << (id % 32));
    io_fence();
}
auto plic_take(usize base, u32 context) noexcept -> u32 {
    io_fence(); auto* claim = word(base, 0x200004 + context * 0x1000); const auto id = *claim;
    // Completion precedes masking; the driver's ACK only rearms the source.
    if (id) *claim = id;
    io_fence(); return id;
}
} // namespace arch
