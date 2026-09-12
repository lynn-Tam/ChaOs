#include <io/device.hpp>

#include <core/debug.hpp>
#include <libk/utility.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel::io {

Device::~Device() noexcept {
    KASSERT(!reserved_);
}

auto Device::acquire(void* context, Stop stop) noexcept -> libk::optional<DeviceLease> {
    sync::IrqLockGuard guard{lock_};
    if (reserved_ || retired_) return libk::nullopt;
    KASSERT((context == nullptr) == (stop == nullptr));
    reserved_ = true;
    context_ = context;
    stop_ = stop;
    return libk::optional<DeviceLease>{DeviceLease{*this}};
}

void Device::retire() noexcept {
    sync::IrqLockGuard guard{lock_};
    if (retired_) return;
    retired_ = true;
    if (stop_) stop_(context_);
}

void Device::release() noexcept {
    sync::IrqLockGuard guard{lock_};
    KASSERT(reserved_);
    reserved_ = false;
    context_ = nullptr;
    stop_ = nullptr;
}

DeviceLease::DeviceLease(DeviceLease&& other) noexcept
    : device_(libk::exchange(other.device_, nullptr)),
      root_(libk::move(other.root_)), state_(other.state_),
      deadline_(other.deadline_), ticket_(other.ticket_) {}

DeviceLease::~DeviceLease() noexcept {
    if (device_ == nullptr) return;
    KASSERT(state_ == State::Reserved || state_ == State::Closed);
    KASSERT(!root_);
    device_->release();
}

auto DeviceLease::bars() const noexcept -> const libk::Array<arch::PciBar, 6>& {
    KASSERT(device_ != nullptr);
    return device_->function_.bars();
}

auto DeviceLease::config32(u16 offset) const noexcept -> u32 {
    KASSERT(device_ != nullptr);
    KASSERT(state_ == State::Reserved || state_ == State::Active);
    return device_->function_.config32(offset);
}

auto DeviceLease::irq_source() const noexcept -> u32 {
    KASSERT(device_ != nullptr);
    return device_->function_.irq_source();
}

auto DeviceLease::configuration() const noexcept -> const libk::Array<u32, 64>& {
    KASSERT(device_ != nullptr);
    return device_->function_.configuration();
}

auto DeviceLease::take_fault() noexcept -> libk::optional<arch::IoFault> {
    KASSERT(device_ != nullptr);
    return device_->iommu_.take_fault();
}

auto DeviceLease::deadline(u64 nanoseconds) noexcept -> bool {
    const auto duration = device_->clock_.duration_from_nanoseconds(nanoseconds);
    const auto end = duration ? device_->clock_.now().checked_add(*duration)
                              : libk::nullopt;
    if (!end) {
        fail();
        return false;
    }
    deadline_ = *end;
    return true;
}

void DeviceLease::open(arch::IoRoot&& root) noexcept {
    KASSERT(device_ != nullptr && state_ == State::Reserved);
    KASSERT(root.page_count() != 0);
    root_.emplace(libk::move(root));
    state_ = State::Opening;
    if (!deadline(1'000'000'000)) return;
    const auto issued = device_->iommu_.replace(root_->page());
    if (!issued) {
        fail();
        return;
    }
    ticket_ = issued.value();
}

void DeviceLease::reset_device() noexcept {
    // BAR retirement precedes this call. A device-memory read drains prior CPU
    // posted writes while decode is still enabled; virtio reset drains the selected
    // QEMU virtio backend. IOFENCE alone cannot prove backend quiescence.
    device_->function_.disable_dma();
    device_->function_.flush_mmio();
    device_->function_.begin_reset();
    state_ = State::Resetting;
    static_cast<void>(deadline(1'000'000'000));
}

void DeviceLease::close() noexcept {
    KASSERT(device_ != nullptr);
    switch (state_) {
    case State::Reserved: state_ = State::Closed; break;
    case State::Opening: state_ = State::ClosingOpening; break;
    case State::Active: reset_device(); break;
    default: break;
    }
}

auto DeviceLease::poll() noexcept -> State {
    KASSERT(device_ != nullptr);
    switch (state_) {
    case State::Opening:
    case State::ClosingOpening:
    case State::Invalidating: {
        const auto completion = device_->iommu_.poll(ticket_);
        if (completion == arch::IoStatus::Failed) {
            fail();
        } else if (completion == arch::IoStatus::Pending) {
            if (device_->clock_.now() >= deadline_) fail();
        } else if (state_ == State::Opening) {
            device_->function_.enable_dma();
            state_ = State::Active;
        } else if (state_ == State::ClosingOpening) {
            reset_device();
        } else {
            if (device_->iommu_.clear_faults() != arch::IoStatus::Complete) {
                fail();
                break;
            }
            root_.reset();
            state_ = State::Closed;
        }
        break;
    }
    case State::Resetting:
        if (!device_->function_.reset_complete()) {
            if (device_->clock_.now() >= deadline_) fail();
            break;
        }
        if (const auto issued = device_->iommu_.replace(libk::nullopt)) {
            ticket_ = issued.value();
            state_ = State::Invalidating;
            static_cast<void>(deadline(1'000'000'000));
        } else {
            fail();
        }
        break;
    default: break;
    }
    return state_;
}

} // namespace kernel::io
