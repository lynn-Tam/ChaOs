#pragma once

#include <optional>
#include <utility>


#include <io/host.hpp>
#include <array>
#include <uapi/io.h>
#include <atomic>
#include <ranges>
#include "fdt.hpp"
#include <time/clock.hpp>

class Iommu;

class PciDma final : public io::Hw {
public:
    PciDma(u16 requester, u16 did, const FwPci&, Iommu&, const time::Clock&, irq::Line) noexcept;
    ~PciDma() noexcept = default;
    auto reserve() noexcept -> bool override;
    auto state() const noexcept -> State override;
    auto device_id() const noexcept -> u16 { return did_; }
    auto requester() const noexcept -> u16 override { return requester_; }
    auto regs() const noexcept -> std::span<const io::Reg> override { return regs_; }
    auto irq() const noexcept -> irq::Line override { return line_; }
    auto take_fault() noexcept -> std::optional<io::Fault> override;
    void open(mm::PageTable&& root) noexcept override;
    void close() noexcept override;
    auto poll() noexcept -> State override;
private:
    friend class Iommu;
    enum class Phase : u8 { Reserved, Opening, Active, ClosingOpening, Resetting, Invalidating, Draining, Closed, Failed };
    void reset_device() noexcept;
    void fail() noexcept { phase_ = Phase::Failed; }
    auto deadline(u64 nanoseconds) noexcept -> bool;
    u16 requester_, did_;
    const FwPci& pci_;
    usize status_{};
    std::array<io::Reg, IO_REG_COUNT> regs_{};
    Iommu& iommu_;
    const time::Clock& clock_;
    irq::Line line_;
    std::optional<mm::PageTable> root_{};
    std::atomic<Phase> phase_{Phase::Reserved};
public:
    PciDma* next{};
private:
    // Fault fields are protected by Iommu::lock_; identity links never change.
    std::optional<io::Fault> fault_{};
    bool recovering_{};
    time::Instant deadline_{};
};

// The board owns this controller for its entire lifetime. No driver
// receives register access. One controller owns the command and fault queues;
// each root-bus requester has its own device context and fault mailbox.
// Published queue/directory pages remain owned even on hardware failure.
class Iommu final : private libk::noncopyable_nonmovable {
public:
    enum class Step : u8 { Pending, Complete, Failed };
    enum class Error : u8 { Absent, Unsupported, NoMemory, Busy, Failed };
    explicit Iommu(usize base) noexcept : base_(base) {}
    ~Iommu() noexcept;
    [[nodiscard]] auto start(mm::Pmm& pmm) noexcept
        -> std::expected<void, Error>;
    [[nodiscard]] auto initialize() noexcept -> Step;
    auto devices() noexcept -> PciDma* { return devices_.load(std::memory_order_acquire); }
    void add(PciDma&) noexcept;
    [[nodiscard]] auto interrupt() noexcept -> bool;

private:
    friend class PciDma;

    auto take_fault(PciDma&) noexcept -> std::optional<io::Fault>;
    // Exactly one device owns the in-flight hardware fence. Completion is
    // consumed by that same device, so no software ticket/generation is needed.
    auto advance(PciDma&, std::optional<mm::Page> root) noexcept -> Step;
    auto clear_faults(PciDma&) noexcept -> Step;
    template<class T> auto read(usize offset) const noexcept -> T;
    template<class T> void write(usize offset, T value) const noexcept;
    usize base_;
    static constexpr u16 RootBusRequesters = 256;
    static constexpr usize ContextPages = RootBusRequesters / 64;
    enum class State : u8 { Idle, Disabling, Queues, Directory, Ready, Failed };
    [[nodiscard]] auto failed() noexcept -> Step;
    [[nodiscard]] auto drain_faults() noexcept -> Step;
    void begin_overflow() noexcept;
    mm::PageGroup storage_{};
    mm::Page directory_{};
    mm::Page contexts_[ContextPages]{};
    mm::Page commands_{};
    mm::Page faults_{};
    std::atomic<PciDma*> devices_{};
    PciDma* inflight_{};
    enum class FaultMode : u8 { On, Draining, Rearming };
    FaultMode fault_mode_{FaultMode::On};
    mutable sync::Spin lock_{};
    u32 tail_{};
    u32 fault_head_{};
    State state_{State::Idle};
};
