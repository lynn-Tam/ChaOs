#pragma once
#include <expected>
#include <optional>
#include <span>
#include <mm/table.hpp>
#include <irq/route.hpp>

namespace io {
struct Reg { usize pa{}, size{}; mm::Perms perms{}; };
struct Fault { u16 cause{}; u32 requester{}; u64 address{}; };
enum class BindErr : u8 { Busy, Unavailable, NoMemory };

// Machine-lifetime session storage, including failed hardware quarantine.
class Hw : private libk::noncopyable_nonmovable {
public:
    using Stop = libk::delegate<void(bool fault) noexcept>;
    enum class State : u8 { Reserved, Opening, Active, Closing, Closed, Failed };
    auto acquire(Stop stop) noexcept -> std::expected<Hw*, BindErr> {
        sync::Lock guard{gate_};
        libk_assert(stop);
        if (stop_) return std::unexpected(BindErr::Busy);
        if (state() == State::Failed || !reserve()) return std::unexpected(BindErr::Unavailable);
        stop_ = stop;
        return this;
    }
    void release() noexcept {
        sync::Lock guard{gate_};
        libk_assert(state() == State::Reserved || state() == State::Closed);
        stop_.reset();
    }
    void signal_fault() noexcept { sync::Lock guard{gate_}; if (stop_) stop_(true); }
    virtual auto state() const noexcept -> State = 0;
    virtual auto requester() const noexcept -> u16 = 0;
    virtual auto regs() const noexcept -> std::span<const Reg> = 0;
    virtual auto irq() const noexcept -> irq::Line = 0;
    virtual auto take_fault() noexcept -> std::optional<Fault> = 0;
    virtual void open(mm::PageTable&&) noexcept = 0;
    virtual void close() noexcept = 0;
    virtual auto poll() noexcept -> State = 0;
protected:
    virtual auto reserve() noexcept -> bool = 0;
    ~Hw() noexcept = default;
private:
    sync::Spin gate_{};
    Stop stop_{};
};

class Bus {
public:
    explicit Bus(usize count) noexcept : count(count) {}
    const usize count;
    virtual auto acquire(usize requester, Hw::Stop) noexcept -> std::expected<Hw*, BindErr> = 0;
protected:
    ~Bus() noexcept = default;
};

// Capability owner; board code owns only the borrowed hardware backend.
class Host final : private libk::noncopyable_nonmovable {
public:
    explicit Host(Bus& bus) noexcept : bus_(bus) {}
    auto acquire(usize requester, Hw::Stop stop) noexcept -> std::expected<Hw*, BindErr> {
        sync::Lock guard{gate_};
        if (closed_ || requester >= bus_.count) return std::unexpected(BindErr::Unavailable);
        return bus_.acquire(requester, stop);
    }
    void retire() noexcept { sync::Lock guard{gate_}; closed_ = true; }
private:
    Bus& bus_;
    sync::Spin gate_{};
    bool closed_{};
};
} // namespace io
