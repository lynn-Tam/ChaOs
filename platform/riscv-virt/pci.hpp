#pragma once

#include <base/types.hpp>
#include <array>
#include <expected>
#include <io/types.hpp>
#include <libk/delegate.hpp>
#include <libk/noncopyable.hpp>

enum class PciError : u8 { Absent, Unsupported, Invalid };

// QEMU virt root-bus modern virtio-blk endpoints. The kernel Device owns each
// handle; BAR addresses are allocated from one platform-wide MMIO window.
// a userspace driver never receives ECAM authority. All methods require the
// Device owner's serialization, including across IOSpace lease generations.
class PciFn final : private libk::noncopyable {
public:
    using Found = libk::delegate<bool(PciFn&&) noexcept>;
    PciFn(PciFn&&) noexcept = default;
    auto operator=(PciFn&&) noexcept -> PciFn& = default;

    [[nodiscard]] static auto scan(Found found) noexcept
        -> std::expected<void, PciError>;
    [[nodiscard]] auto requester() const noexcept -> u16 { return requester_; }
    [[nodiscard]] auto irq_source() const noexcept -> u32 { return irq_source_; }
    [[nodiscard]] auto configuration() const noexcept -> const std::array<u32, 64>& {
        return configuration_;
    }
    [[nodiscard]] auto bars() const noexcept -> const std::array<io::Bar, 6>& {
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
    PciFn() noexcept = default;
    [[nodiscard]] auto configure_bars(usize& next) noexcept
        -> std::expected<void, PciError>;
    usize config_{};
    usize status_{};
    u16 requester_{};
    u32 irq_source_{};
    std::array<io::Bar, 6> bars_{};
    std::array<u32, 64> configuration_{};
};
