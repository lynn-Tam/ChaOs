#pragma once

#include <expected>
#include <libk/noncopyable.hpp>
#include <optional>
#include <mm/pmm.hpp>
#include <io/types.hpp>
#include <sync.hpp>

namespace arch {

enum class IoStatus : u8 { Pending, Complete, Failed };
enum class IommuError : u8 { Absent, Unsupported, InsufficientMemory, Busy, Failed };

// The kernel platform owns this controller for its entire lifetime. No driver
// receives register access. One controller owns the command and fault queues;
// each root-bus requester has its own device context and fault mailbox.
// Published queue/directory pages remain owned even on hardware failure.
class Iommu final : private libk::noncopyable_nonmovable {
public:
    explicit Iommu(usize base) noexcept : base_(base) {}
    ~Iommu() noexcept;
    [[nodiscard]] auto start(mm::Pmm& pmm) noexcept
        -> std::expected<void, IommuError>;
    [[nodiscard]] auto initialize() noexcept -> IoStatus;
    // root == nullopt denies all DMA. Caller stops the device before changing
    // a live context and retains old mappings until this ticket completes.
    [[nodiscard]] auto replace(u16 requester,
        std::optional<mm::Page> root) noexcept
        -> std::expected<u64, IommuError>;
    [[nodiscard]] auto poll(u64 ticket) noexcept -> IoStatus;
    // Called from the platform fault interrupt. A failed queue invalidates
    // every device context and must be handled at controller scope.
    [[nodiscard]] auto handle_fault_irq() noexcept -> IoStatus;
    [[nodiscard]] auto fault_pending(u16 requester) noexcept -> bool;
    [[nodiscard]] auto fault_overflow() noexcept -> bool;
    [[nodiscard]] auto take_fault(u16 requester) noexcept -> std::optional<io::Fault>;
    // Only after device reset and the invalidation fence have completed: no
    // producer from the old lease remains. Fault records contain no software
    // generation, so they must not leak into the next device lease.
    [[nodiscard]] auto clear_faults(u16 requester) noexcept -> IoStatus;

private:
    template<class T> auto read(usize offset) const noexcept -> T;
    template<class T> void write(usize offset, T value) const noexcept;
    usize base_;
    static constexpr u16 RootBusRequesters = 256;
    static constexpr usize ContextPages = RootBusRequesters / 64;
    enum class State : u8 { Idle, Disabling, Queues, Directory, Ready, Failed };
    [[nodiscard]] auto failed() noexcept -> IoStatus;
    [[nodiscard]] auto drain_faults() noexcept -> IoStatus;
    void begin_overflow() noexcept;
    mm::PageGroup storage_{};
    mm::Page directory_{};
    mm::Page contexts_[ContextPages]{};
    mm::Page commands_{};
    mm::Page faults_{};
    std::optional<io::Fault> pending_faults_[RootBusRequesters]{};
    bool active_[RootBusRequesters]{};
    bool recovering_[RootBusRequesters]{};
    usize recovery_count_{};
    bool overflow_{};
    bool rearming_{};
    mutable sync::Spin lock_{};
    u32 tail_{};
    u32 fault_head_{};
    u64 issued_{};
    u64 completed_{};
    State state_{State::Idle};
};

} // namespace arch
