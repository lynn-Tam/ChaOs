#pragma once

#include <libk/expected.hpp>
#include <libk/noncopyable.hpp>
#include <libk/optional.hpp>
#include <mm/pmm.hpp>
#include <sync/lock.hpp>

namespace arch {

enum class IoStatus : u8 { Pending, Complete, Failed };
enum class IommuError : u8 { Absent, Unsupported, InsufficientMemory, Busy, Failed };

struct IoFault final {
    u16 cause{};
    u32 requester{};
    u64 address{};
};

// The kernel platform owns this controller for its entire lifetime. No driver
// receives register access. One controller owns the command and fault queues;
// each root-bus requester has its own device context and fault mailbox.
// Published queue/directory pages remain owned even on hardware failure.
class Iommu final : private libk::noncopyable_nonmovable {
public:
    Iommu() noexcept = default;
    ~Iommu() noexcept;
    [[nodiscard]] auto start(kernel::mm::Pmm& pmm) noexcept
        -> libk::Expected<void, IommuError>;
    [[nodiscard]] auto initialize() noexcept -> IoStatus;
    // root == nullopt denies all DMA. Caller stops the device before changing
    // a live context and retains old mappings until this ticket completes.
    [[nodiscard]] auto replace(u16 requester,
        libk::optional<kernel::mm::Page> root) noexcept
        -> libk::Expected<u64, IommuError>;
    [[nodiscard]] auto poll(u64 ticket) noexcept -> IoStatus;
    // Called from the platform fault interrupt. A failed queue invalidates
    // every device context and must be handled at controller scope.
    [[nodiscard]] auto handle_fault_irq() noexcept -> IoStatus;
    [[nodiscard]] auto fault_pending(u16 requester) noexcept -> bool;
    [[nodiscard]] auto fault_overflow() noexcept -> bool;
    [[nodiscard]] auto take_fault(u16 requester) noexcept -> libk::optional<IoFault>;
    // Only after device reset and the invalidation fence have completed: no
    // producer from the old lease remains. Fault records contain no software
    // generation, so they must not leak into the next device lease.
    [[nodiscard]] auto clear_faults(u16 requester) noexcept -> IoStatus;

private:
    static constexpr u16 RootBusRequesters = 256;
    static constexpr usize ContextPages = RootBusRequesters / 64;
    enum class State : u8 { Idle, Disabling, Queues, Directory, Ready, Failed };
    [[nodiscard]] auto failed() noexcept -> IoStatus;
    [[nodiscard]] auto drain_faults() noexcept -> IoStatus;
    void begin_overflow() noexcept;
    kernel::mm::OwnedPageGroup storage_{};
    kernel::mm::Page directory_{};
    kernel::mm::Page contexts_[ContextPages]{};
    kernel::mm::Page commands_{};
    kernel::mm::Page faults_{};
    libk::optional<IoFault> pending_faults_[RootBusRequesters]{};
    bool active_[RootBusRequesters]{};
    bool recovering_[RootBusRequesters]{};
    usize recovery_count_{};
    bool overflow_{};
    bool rearming_{};
    mutable kernel::sync::SpinLock<kernel::sync::LockClass::Iommu> lock_{};
    u32 tail_{};
    u32 fault_head_{};
    u64 issued_{};
    u64 completed_{};
    State state_{State::Idle};
};

} // namespace arch
