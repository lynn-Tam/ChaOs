#pragma once

#include <optional>
#include <utility>


#include "pci.hpp"
#include <io/device.hpp>
#include <atomic>
#include <ranges>
#include <object/ref.hpp>
#include <libk/intrusive_list.hpp>
#include <time/clock.hpp>

class Iommu;

class PciDma final : public io::Hw {
public:
    PciDma(PciFn&&, Iommu&, const time::Clock&, irq::Line) noexcept;
    ~PciDma() noexcept;
    object::ref<io::Device> device{};
    void reserve() noexcept override;
    auto state() const noexcept -> State override;
    auto requester() const noexcept -> u16 override { return function_.requester(); }
    auto info() const noexcept -> io::DeviceInfo override;
    auto bars() const noexcept -> const std::array<io::Bar, 6>& override { return function_.bars(); }
    auto config32(u16 offset) const noexcept -> u32 override { return function_.config32(offset); }
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
    PciFn function_;
    Iommu& iommu_;
    const time::Clock& clock_;
    irq::Line line_;
    std::optional<mm::PageTable> root_{};
    std::atomic<Phase> phase_{Phase::Reserved};
    libk::IntrusiveListHook hook_{};
    // Accessed under Iommu::lock_; the registration list is frozen before IRQ enable.
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
    // Registration is startup-only; the list and hardware identities then outlive sessions.
    auto devices() noexcept { return std::ranges::subrange(devices_.begin(), devices_.end()); }
    [[nodiscard]] auto interrupt() noexcept -> bool;

private:
    friend class PciDma;
    void add(PciDma&) noexcept;
    void remove(PciDma&) noexcept;
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
    libk::IntrusiveList<PciDma, &PciDma::hook_> devices_{};
    PciDma* inflight_{};
    enum class FaultMode : u8 { On, Draining, Rearming };
    FaultMode fault_mode_{FaultMode::On};
    mutable sync::Spin lock_{};
    u32 tail_{};
    u32 fault_head_{};
    State state_{State::Idle};
};
