#pragma once

#include <optional>
#include <utility>


#include "pci.hpp"
#include <io/device.hpp>
#include <arch/iommu.hpp>
#include <time/clock.hpp>

class PciDma final : public io::Hw {
public:
    PciDma(PciFn&& fn, arch::Iommu& iommu, const time::Clock& clock, irq::Line line) noexcept
        : function_(std::move(fn)), iommu_(iommu), clock_(clock), line_(line) {}
    void reserve() noexcept override;
    auto state() const noexcept -> State override;
    auto requester() const noexcept -> u16 override { return function_.requester(); }
    auto info() const noexcept -> io::DeviceInfo override;
    auto bars() const noexcept -> const std::array<io::Bar, 6>& override { return function_.bars(); }
    auto config32(u16 offset) const noexcept -> u32 override { return function_.config32(offset); }
    auto irq() const noexcept -> irq::Line override { return line_; }
    auto take_fault() noexcept -> std::optional<io::Fault> override { return iommu_.take_fault(function_.requester()); }
    void open(mm::PageTable&& root) noexcept override;
    void close() noexcept override;
    auto poll() noexcept -> State override;
private:
    enum class Phase : u8 { Reserved, Opening, Active, ClosingOpening, Resetting, Invalidating, Closed, Failed };
    void reset_device() noexcept;
    void fail() noexcept { phase_ = Phase::Failed; }
    auto deadline(u64 nanoseconds) noexcept -> bool;
    PciFn function_;
    arch::Iommu& iommu_;
    const time::Clock& clock_;
    irq::Line line_;
    std::optional<mm::PageTable> root_{};
    Phase phase_{Phase::Reserved};
    time::Instant deadline_{};
    u64 ticket_{};
};
