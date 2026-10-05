#pragma once

#include <optional>


#include <io/types.hpp>
#include <mm/table.hpp>
#include <irq/route.hpp>

namespace io {

// Machine-owned backend. One exclusive lease drives its DMA session;
// the concrete owner also retains failed-session quarantine storage.
class Hw : private libk::noncopyable_nonmovable {
public:
    enum class State : u8 { Reserved, Opening, Active, Closing, Closed, Failed };
    virtual void reserve() noexcept = 0;
    [[nodiscard]] virtual auto state() const noexcept -> State = 0;
    [[nodiscard]] virtual auto requester() const noexcept -> u16 = 0;
    [[nodiscard]] virtual auto info() const noexcept -> DeviceInfo = 0;
    [[nodiscard]] virtual auto bars() const noexcept -> const std::array<Bar, 6>& = 0;
    [[nodiscard]] virtual auto config32(u16) const noexcept -> u32 = 0;
    [[nodiscard]] virtual auto irq() const noexcept -> irq::Line = 0;
    [[nodiscard]] virtual auto take_fault() noexcept -> std::optional<Fault> = 0;
    virtual void open(mm::PageTable&&) noexcept = 0;
    virtual void close() noexcept = 0;
    [[nodiscard]] virtual auto poll() noexcept -> State = 0;
protected:
    ~Hw() noexcept = default;
};

class Device;

class DeviceLease final : private libk::noncopyable {
public:
    using State = Hw::State;
    DeviceLease(DeviceLease&& other) noexcept;
    auto operator=(DeviceLease&&) -> DeviceLease& = delete;
    ~DeviceLease() noexcept;
    [[nodiscard]] auto state() const noexcept -> State;
    [[nodiscard]] auto bars() const noexcept -> const std::array<Bar, 6>&;
    [[nodiscard]] auto config32(u16 offset) const noexcept -> u32;
    [[nodiscard]] auto irq_source() const noexcept -> irq::Line;
    [[nodiscard]] auto take_fault() noexcept -> std::optional<Fault>;
    void open(mm::PageTable&& root) noexcept;
    void close() noexcept;
    [[nodiscard]] auto poll() noexcept -> State;
private:
    friend class Device;
    explicit DeviceLease(Device& device) noexcept : device_(&device) {}
    Device* device_{};
};

// Capability identity and exclusive admission. The hardware backend owns
// protocol phases; IOSpace owns the DMA, BAR and IRQ capability generation.
class Device final : private libk::noncopyable_nonmovable {
public:
    using Stop = libk::delegate<void(bool fault) noexcept>;
    explicit Device(Hw& hw) noexcept : hw_(hw) {}
    ~Device() noexcept;
    [[nodiscard]] auto acquire(Stop stop = {}) noexcept -> std::optional<DeviceLease>;
    void retire() noexcept;
    void signal_fault() noexcept;
    [[nodiscard]] auto requester() const noexcept -> u16 { return hw_.requester(); }
    [[nodiscard]] auto info() const noexcept -> DeviceInfo { return hw_.info(); }
private:
    friend class DeviceLease;
    void release() noexcept;
    Hw& hw_;
    mutable sync::Spin lock_{};
    bool reserved_{};
    bool retired_{};
    Stop stop_{};
};

} // namespace io
