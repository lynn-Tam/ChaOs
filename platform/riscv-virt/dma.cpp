#include <utility>
#include "dma.hpp"

void PciDma::reserve() noexcept {
    libk_assert((phase_ == Phase::Reserved || phase_ == Phase::Closed) && !root_);
    phase_ = Phase::Reserved;
    ticket_ = 0;
}

auto PciDma::state() const noexcept -> State {
    switch (phase_) {
    case Phase::Reserved: return State::Reserved;
    case Phase::Opening: return State::Opening;
    case Phase::Active: return State::Active;
    case Phase::Closed: return State::Closed;
    case Phase::Failed: return State::Failed;
    default: return State::Closing;
    }
}

auto PciDma::info() const noexcept -> io::DeviceInfo {
    io::DeviceInfo info{.requester = function_.requester(), .configuration = function_.configuration()};
    for (usize i = 0; i < info.bar_sizes.size(); ++i) info.bar_sizes[i] = function_.bars()[i].size;
    return info;
}

auto PciDma::deadline(u64 nanoseconds) noexcept -> bool {
    const auto duration = clock_.duration_from_nanoseconds(nanoseconds);
    const auto end = duration ? clock_.now().checked_add(*duration)
                              : std::nullopt;
    if (!end) {
        fail();
        return false;
    }
    deadline_ = *end;
    return true;
}

void PciDma::open(mm::PageTable&& root) noexcept {
    libk_assert(phase_ == Phase::Reserved);
    libk_assert(root.kind() == mm::PageTable::Kind::Io);
    root_.emplace(std::move(root));
    phase_ = Phase::Opening;
    if (!deadline(1'000'000'000)) return;
    const auto issued = iommu_.replace(function_.requester(), root_->page());
    if (!issued && issued.error() != arch::IommuError::Busy) {
        fail();
        return;
    }
    if (issued) ticket_ = issued.value();
}

void PciDma::reset_device() noexcept {
    // BAR retirement precedes this call. A device-memory read drains prior CPU
    // posted writes while decode is still enabled; virtio reset drains the selected
    // QEMU virtio backend. IOFENCE alone cannot prove backend quiescence.
    function_.disable_dma();
    function_.flush_mmio();
    function_.begin_reset();
    phase_ = Phase::Resetting;
    static_cast<void>(deadline(1'000'000'000));
}

void PciDma::close() noexcept {
    switch (phase_) {
    case Phase::Reserved: phase_ = Phase::Closed; break;
    case Phase::Opening: phase_ = Phase::ClosingOpening; break;
    case Phase::Active: reset_device(); break;
    default: break;
    }
}

auto PciDma::poll() noexcept -> State {
    switch (phase_) {
    case Phase::Opening:
    case Phase::ClosingOpening:
    case Phase::Invalidating: {
        if (ticket_ == 0) {
            if (phase_ == Phase::ClosingOpening) {
                reset_device();
                break;
            }
            const auto issued = iommu_.replace(function_.requester(), root_->page());
            if (issued) ticket_ = issued.value();
            else if (issued.error() != arch::IommuError::Busy) fail();
            if (ticket_ == 0) {
                if (clock_.now() >= deadline_) fail();
                break;
            }
        }
        const auto completion = iommu_.poll(ticket_);
        if (completion == arch::IoStatus::Failed) {
            fail();
        } else if (completion == arch::IoStatus::Pending) {
            if (clock_.now() >= deadline_) fail();
        } else if (phase_ == Phase::Opening) {
            function_.enable_dma();
            phase_ = Phase::Active;
        } else if (phase_ == Phase::ClosingOpening) {
            reset_device();
        } else {
            const auto cleared = iommu_.clear_faults(function_.requester());
            if (cleared == arch::IoStatus::Pending) break;
            if (cleared == arch::IoStatus::Failed) {
                fail();
                break;
            }
            root_.reset();
            phase_ = Phase::Closed;
        }
        break;
    }
    case Phase::Resetting:
        if (!function_.reset_complete()) {
            if (clock_.now() >= deadline_) fail();
            break;
        }
        if (const auto issued = iommu_.replace(function_.requester(), std::nullopt)) {
            ticket_ = issued.value();
            phase_ = Phase::Invalidating;
            static_cast<void>(deadline(1'000'000'000));
        } else if (issued.error() != arch::IommuError::Busy) {
            fail();
        } else if (clock_.now() >= deadline_) {
            fail();
        }
        break;
    default: break;
    }
    return state();
}
