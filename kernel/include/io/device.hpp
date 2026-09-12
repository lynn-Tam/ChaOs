#pragma once

#include <arch/io_page_table.hpp>
#include <arch/iommu.hpp>
#include <arch/pci.hpp>
#include <sync/lock.hpp>
#include <time/clock.hpp>

namespace kernel::io {

class Device;

// IOSpace owns this exclusive hardware lease and serializes its operations.
// It must retain the mapped MemoryObject leases until Closed. Failed hardware
// remains quarantined with this token, its tables and all backing references.
class DeviceLease final : private libk::noncopyable {
public:
    enum class State : u8 {
        Reserved, Opening, Active, ClosingOpening, Resetting, Invalidating,
        Closed, Failed,
    };

    DeviceLease(DeviceLease&& other) noexcept;
    auto operator=(DeviceLease&&) -> DeviceLease& = delete;
    ~DeviceLease() noexcept;

    [[nodiscard]] auto state() const noexcept -> State { return state_; }
    [[nodiscard]] auto bars() const noexcept -> const libk::Array<arch::PciBar, 6>&;
    [[nodiscard]] auto config32(u16 offset) const noexcept -> u32;
    [[nodiscard]] auto irq_source() const noexcept -> u32;
    [[nodiscard]] auto configuration() const noexcept -> const libk::Array<u32, 64>&;
    [[nodiscard]] auto take_fault() noexcept -> libk::optional<arch::IoFault>;
    // Transfers table ownership before publishing the context. Failure after
    // publication cannot return the tables or release the device reservation.
    void open(arch::IoRoot&& root) noexcept;
    // Caller has retired generation BAR/IRQ authority and awaited CPU mapping
    // retirement. Closing during Opening never enables bus mastering.
    void close() noexcept;
    [[nodiscard]] auto poll() noexcept -> State;

private:
    friend class Device;
    explicit DeviceLease(Device& device) noexcept : device_(&device) {}
    void reset_device() noexcept;
    void fail() noexcept { state_ = State::Failed; }
    [[nodiscard]] auto deadline(u64 nanoseconds) noexcept -> bool;

    Device* device_{};
    libk::optional<arch::IoRoot> root_{};
    State state_{State::Reserved};
    time::Instant deadline_{};
    u64 ticket_{};
};

// Platform-lifetime identity. Reservation is the only shared admission state;
// the move-only lease owns the hardware phase and is never duplicated.
class Device final : private libk::noncopyable_nonmovable {
public:
    using Stop = void (*)(void*) noexcept;
    Device(arch::PciFunction&& function, arch::Iommu& iommu,
        const time::Clock& clock) noexcept
        : function_(libk::move(function)), iommu_(iommu), clock_(clock) {}
    ~Device() noexcept;
    // The containing IOSpace retains this Device's structural ObjectHold.
    // stop only records cancellation and queues work; it must not acquire an
    // IOSpace lock. The device lock pins context through the callback.
    [[nodiscard]] auto acquire(void* context = nullptr, Stop stop = nullptr) noexcept
        -> libk::optional<DeviceLease>;
    void retire() noexcept;
    [[nodiscard]] auto requester() const noexcept -> u16 { return function_.requester(); }

private:
    friend class DeviceLease;
    arch::PciFunction function_;
    arch::Iommu& iommu_;
    const time::Clock& clock_;
    void release() noexcept;
    mutable sync::SpinLock<sync::LockClass::Device> lock_{};
    bool reserved_{};
    bool retired_{};
    void* context_{};
    Stop stop_{};
};

} // namespace kernel::io
