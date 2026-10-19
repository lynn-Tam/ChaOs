#pragma once
#include <protocols/msg.hpp>
#include <string.h>

// Data and End share the ordinary bounded ch_pair frame. End is ordered after
// all accepted bytes; its status is the producer's final outcome.
enum class str_frame : uint64_t { Data, End };

class str_reader final {
    msg_rx input_;
    cap_t events_{};
    bool ended_{};
    status_t status_{};

  public:
    [[nodiscard]] explicit operator bool() const noexcept { return bool(input_); }
    auto close() noexcept -> status_t {
        const auto status = input_.close();
        if (status == STATUS_OK) { events_ = 0; ended_ = false; status_ = STATUS_OK; }
        return status;
    }
    auto open(cap_t channel, cap_t events) noexcept -> status_t {
        auto status = close();
        if (status == STATUS_OK) status = input_.open(channel, events);
        if (status != STATUS_OK) return status;
        events_ = events;
        return STATUS_OK;
    }
    auto read(msg_frame& message) noexcept -> status_t {
        message = {};
        if (ended_)
            return status_;
        for (;;) {
            const auto received = input_.try_receive(message);
            if (received.status == STATUS_OK) {
                if (message.operation == static_cast<uint64_t>(str_frame::End)) {
                    if (message.size != 0)
                        return STATUS_PEER_FAULT;
                    ended_ = true;
                    status_ = static_cast<status_t>(message.status);
                    return status_;
                }
                if (message.operation != static_cast<uint64_t>(str_frame::Data) ||
                    message.size == 0)
                    return STATUS_PEER_FAULT;
                return STATUS_OK;
            }
            if (received.status != STATUS_WOULD_BLOCK)
                return received.status;
            const auto armed = input_.arm();
            if (armed.status != STATUS_OK)
                return armed.status;
            const auto wake = ev_wait(events_);
            if (wake.status != STATUS_OK)
                return wake.status;
        }
    }
};

// The channel supplies bounded backpressure. No reply queue is shared by
// writers; console access grants only the ability to enqueue output.
class str_writer final {
    cap_t output_;

  public:
    explicit str_writer(cap_t output) noexcept : output_(output) {}
    void write(const char* text, size_t size) const noexcept {
        while (size != 0) {
            msg_frame message{};
            message.size = size < sizeof(message.data) ? size : sizeof(message.data);
            ::memcpy(message.data, text, message.size);
            if (const auto rc = msg_send(output_, message).status; rc != STATUS_OK)
                sys::exit(rc);
            text += message.size;
            size -= message.size;
        }
    }
    void write(const char* text) const noexcept { write(text, ::strlen(text)); }
    void put(char value) const noexcept { write(&value, 1); }
};

inline constexpr rt_req uart_mem{"uart.memory", 0x55415254, ObjMem};
inline constexpr rt_req uart_irq{"uart.irq", 0x55415254, ObjIrq};
inline constexpr rt_req con_output{"console.output", 0x434f4e53, ObjChannel, 4, 1};
inline constexpr rt_req con_input{"console.input", 0x434f4e53, ObjChannel, 4};
inline constexpr rt_req rt_stdin{"stdin", 0x5354524d, ObjChannel, 4};
inline constexpr rt_req rt_stdout{"stdout", 0x5354524d, ObjChannel, 4};
inline constexpr rt_req rt_stderr{"stderr", 0x5354524d, ObjChannel, 4};
enum class con_operation : uint64_t { Bytes, Prompt };
