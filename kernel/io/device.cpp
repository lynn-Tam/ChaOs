#include <optional>
#include <io/device.hpp>
#include <utility>

namespace io {

Device::~Device() noexcept { libk_assert(!reserved_); }

auto Device::acquire(Stop stop) noexcept -> std::optional<DeviceLease> {
    sync::Lock guard{lock_};
    if (reserved_ || retired_) return std::nullopt;
    hw_.reserve();
    reserved_ = true;
    stop_ = stop;
    return std::optional<DeviceLease>{DeviceLease{*this}};
}

void Device::retire() noexcept {
    sync::Lock guard{lock_};
    if (retired_) return;
    retired_ = true;
    if (stop_) stop_(false);
}

void Device::signal_fault() noexcept {
    sync::Lock guard{lock_};
    if (stop_) stop_(true);
}

void Device::release() noexcept {
    sync::Lock guard{lock_};
    libk_assert(reserved_);
    reserved_ = false;
    stop_.reset();
}

DeviceLease::DeviceLease(DeviceLease&& other) noexcept
    : device_(std::exchange(other.device_, nullptr)) {}

DeviceLease::~DeviceLease() noexcept {
    if (!device_) return;
    libk_assert(state() == State::Reserved || state() == State::Closed);
    device_->release();
}

auto DeviceLease::state() const noexcept -> State { return device_->hw_.state(); }
auto DeviceLease::bars() const noexcept -> const std::array<Bar, 6>& { return device_->hw_.bars(); }
auto DeviceLease::config32(u16 offset) const noexcept -> u32 { return device_->hw_.config32(offset); }
auto DeviceLease::irq_source() const noexcept -> irq::Line { return device_->hw_.irq(); }
auto DeviceLease::take_fault() noexcept -> std::optional<Fault> { return device_->hw_.take_fault(); }
void DeviceLease::open(mm::PageTable&& root) noexcept { device_->hw_.open(std::move(root)); }
void DeviceLease::close() noexcept { device_->hw_.close(); }
auto DeviceLease::poll() noexcept -> State { return device_->hw_.poll(); }

} // namespace io
