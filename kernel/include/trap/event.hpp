// kernel/include/trap/event.hpp
// 内核 trap policy 消费的事实值；这里不暴露具体 ISA 的 CSR 语义。

#pragma once

#include <core/types.hpp>
#include <libk/utility.hpp>
#include <libk/variant.hpp>

namespace kernel::trap {

enum class Origin : u8 {
    Kernel,
    User,
};

enum class Exception : u8 {
    Breakpoint,
    Syscall,
    PageFault,
    AccessFault,
    Misaligned,
    IllegalInstruction,
    Unknown,
};

enum class Interrupt : u8 {
    Timer,
    External,
    Software,
    Unknown,
};

enum class Access : u8 {
    None,
    Execute,
    Read,
    Write,
};

struct ExceptionEvent {
    Exception cause{Exception::Unknown};
    Access access{Access::None};
};

struct InterruptEvent {
    Interrupt cause{Interrupt::Unknown};
};

class Event final {
    using EventPayload = libk::variant<ExceptionEvent, InterruptEvent>;

public:
    [[nodiscard]] static auto exception(
        Origin origin,
        Exception cause,
        Access access,
        usize pc,
        usize fault_addr) noexcept -> Event {
        return Event{
            origin,
            pc,
            fault_addr,
            EventPayload{ExceptionEvent{cause, access}},
        };
    }

    [[nodiscard]] static auto interrupt(
        Origin origin,
        Interrupt cause,
        usize pc) noexcept -> Event {
        return Event{
            origin,
            pc,
            0,
            EventPayload{InterruptEvent{cause}},
        };
    }

    [[nodiscard]] auto origin() const noexcept -> Origin { return origin_; }
    [[nodiscard]] auto pc() const noexcept -> usize { return pc_; }
    [[nodiscard]] auto fault_addr() const noexcept -> usize { return fault_addr_; }
    [[nodiscard]] auto exception() const noexcept -> const ExceptionEvent* {
        return libk::get_if<ExceptionEvent>(&payload_);
    }
    [[nodiscard]] auto interrupt() const noexcept -> const InterruptEvent* {
        return libk::get_if<InterruptEvent>(&payload_);
    }

private:
    Event(
        Origin origin,
        usize pc,
        usize fault_addr,
        EventPayload payload) noexcept
        : origin_(origin),
          pc_(pc),
          fault_addr_(fault_addr),
          payload_(libk::move(payload)) {}

    Origin origin_;
    usize pc_;
    usize fault_addr_;
    EventPayload payload_;
};

} // namespace kernel::trap
