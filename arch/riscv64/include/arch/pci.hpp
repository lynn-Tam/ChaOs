#pragma once

#include <core/types.hpp>
#include <array>
#include <libk/expected.hpp>
#include <libk/inplace_vector.hpp>
#include <libk/noncopyable.hpp>

namespace arch {

inline constexpr usize virt_pci_ecam = 0x3000'0000;
inline constexpr usize virt_pci_memory = 0x4000'0000;
inline constexpr usize virt_iommu_base = 0x0301'0000;
inline constexpr u32 virt_pci_irq_first = 32;
inline constexpr u32 virt_pci_irq_count = 4;
// QEMU virt system IOMMU: CQ/FQ/PM/PQ use PLIC sources 36..39.
inline constexpr u32 virt_iommu_fault_irq = 37;

struct PciBar final {
    usize address{};
    usize size{};
    u32 attributes{};
};

enum class PciError : u8 { Absent, Unsupported, Invalid };

// QEMU virt root-bus modern virtio-blk endpoints. The kernel Device owns each
// handle; BAR addresses are allocated from one platform-wide MMIO window.
// a userspace driver never receives ECAM authority. All methods require the
// Device owner's serialization, including across IOSpace lease generations.
class PciFunction final : private libk::noncopyable {
public:
    using Functions = libk::InplaceVector<PciFunction, virt_pci_irq_count>;
    PciFunction(PciFunction&&) noexcept = default;
    auto operator=(PciFunction&&) noexcept -> PciFunction& = default;

    [[nodiscard]] static auto discover_blocks(Functions& output) noexcept
        -> libk::Expected<void, PciError>;
    [[nodiscard]] auto requester() const noexcept -> u16 { return requester_; }
    [[nodiscard]] auto irq_source() const noexcept -> u32 { return irq_source_; }
    [[nodiscard]] auto configuration() const noexcept -> const std::array<u32, 64>& {
        return configuration_;
    }
    [[nodiscard]] auto bars() const noexcept -> const std::array<PciBar, 6>& {
        return bars_;
    }
    [[nodiscard]] auto config32(u16 offset) const noexcept -> u32;
    // Only valid after the IOMMU context and all DMA mappings are published.
    void enable_dma() const noexcept;
    void disable_dma() const noexcept;
    // Caller first retires every userspace BAR mapping. The read is from the
    // virtio common configuration (device_status), not PCI configuration space.
    void flush_mmio() const noexcept;
    void begin_reset() const noexcept;
    // Virtio device_status == 0 acknowledges device reset, including queues.
    // This is the selected virtio function protocol, not generic PCIe FLR.
    [[nodiscard]] auto reset_complete() const noexcept -> bool;

private:
    PciFunction() noexcept = default;
    [[nodiscard]] auto configure_bars(usize& next) noexcept
        -> libk::Expected<void, PciError>;
    usize config_{};
    usize status_{};
    u16 requester_{};
    u32 irq_source_{};
    std::array<PciBar, 6> bars_{};
    std::array<u32, 64> configuration_{};
};

} // namespace arch
