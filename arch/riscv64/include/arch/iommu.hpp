#pragma once

#include <libk/expected.hpp>
#include <libk/noncopyable.hpp>
#include <libk/optional.hpp>
#include <mm/pmm.hpp>

namespace arch {

enum class IoStatus : u8 { Pending, Complete, Failed };
enum class IommuError : u8 { Absent, Unsupported, InsufficientMemory, Busy, Failed };

struct IoFault final {
    u16 cause{};
    u32 requester{};
    u64 address{};
};

// The kernel platform owns this controller for its entire lifetime. No driver
// receives register access. Current topology has one Device/Requester ID; its
// owner serializes context replacement, command submission and fault draining.
// Published queue/directory pages remain owned even on hardware failure.
class Iommu final : private libk::noncopyable_nonmovable {
public:
    Iommu() noexcept = default;
    ~Iommu() noexcept;
    [[nodiscard]] auto start(kernel::mm::Pmm& pmm, u16 requester) noexcept
        -> libk::Expected<void, IommuError>;
    [[nodiscard]] auto initialize() noexcept -> IoStatus;
    // root == nullopt denies all DMA. Caller stops the device before changing
    // a live context and retains old mappings until this ticket completes.
    [[nodiscard]] auto replace(libk::optional<kernel::mm::Page> root) noexcept
        -> libk::Expected<u64, IommuError>;
    [[nodiscard]] auto poll(u64 ticket) noexcept -> IoStatus;
    [[nodiscard]] auto take_fault() noexcept -> libk::optional<IoFault>;
    // Only after device reset and the invalidation fence have completed: no
    // producer from the old lease remains. Fault records contain no software
    // generation, so they must not leak into the next device lease.
    [[nodiscard]] auto clear_faults() noexcept -> IoStatus;

private:
    enum class State : u8 { Idle, Disabling, Queues, Directory, Ready, Failed };
    [[nodiscard]] auto failed() noexcept -> IoStatus;
    kernel::mm::OwnedPageGroup storage_{};
    kernel::mm::Page directory_{};
    kernel::mm::Page contexts_{};
    kernel::mm::Page commands_{};
    kernel::mm::Page faults_{};
    u64* context_{};
    u16 requester_{};
    u32 tail_{};
    u32 fault_head_{};
    u64 issued_{};
    u64 completed_{};
    State state_{State::Idle};
};

} // namespace arch
