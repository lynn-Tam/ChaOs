#pragma once

#include <core/types.hpp>
#include <libk/array.hpp>
#include <libk/expected.hpp>
#include <libk/noncopyable.hpp>

namespace arch {

inline constexpr usize virt_pci_ecam = 0x3000'0000;
inline constexpr usize virt_pci_memory = 0x4000'0000;
inline constexpr usize virt_iommu_base = 0x0301'0000;
inline constexpr u32 virt_pci_irq_first = 32;
inline constexpr u32 virt_pci_irq_count = 4;

struct PciBar final {
    usize address{};
    usize size{};
    u32 attributes{};
};

enum class PciError : u8 { Absent, Unsupported, Invalid };

// QEMU virt's isolated block-function topology: one modern virtio-blk endpoint
// on the root bus. The kernel Device owns this handle;
// a userspace driver never receives ECAM authority. All methods require the
// Device owner's serialization, including across IOSpace lease generations.
class PciFunction final : private libk::noncopyable {
public:
    PciFunction(PciFunction&&) noexcept = default;
    auto operator=(PciFunction&&) noexcept -> PciFunction& = default;

    [[nodiscard]] static auto discover_block() noexcept
        -> libk::Expected<PciFunction, PciError>;
    [[nodiscard]] auto requester() const noexcept -> u16 { return requester_; }
    [[nodiscard]] auto irq_source() const noexcept -> u32 { return irq_source_; }
    [[nodiscard]] auto configuration() const noexcept -> const libk::Array<u32, 64>& {
        return configuration_;
    }
    [[nodiscard]] auto bars() const noexcept -> const libk::Array<PciBar, 6>& {
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
    usize config_{};
    usize status_{};
    u16 requester_{};
    u32 irq_source_{};
    libk::Array<PciBar, 6> bars_{};
    libk::Array<u32, 64> configuration_{};
};

} // namespace arch
