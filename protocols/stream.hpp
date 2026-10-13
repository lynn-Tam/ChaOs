#pragma once
#include <ipc.hpp>

// Data and End share the ordinary bounded ch_pair frame. End is ordered after
// all accepted bytes; its status is the producer's final outcome.
enum class str_frame : uint64_t { Data, End };

class str_reader final {
    cap_t channel_{}, events_{};
    word_t readable_{};
    uint64_t sequence_{};
    bool ended_{};
    status_t status_{};

  public:
    [[nodiscard]] explicit operator bool() const noexcept { return channel_ != 0; }
    auto close() noexcept -> status_t {
        if (!channel_)
            return STATUS_OK;
        const auto status = sys::channel_unbind(channel_, readable_).status;
        if (status == STATUS_OK)
            *this = {};
        return status;
    }
    auto open(cap_t channel, cap_t events) noexcept -> status_t {
        const auto result = sys::channel_bind(channel, events, CHANNEL_READABLE);
        if (result.status != STATUS_OK)
            return result.status;
        channel_ = channel;
        events_ = events;
        readable_ = result.value;
        return STATUS_OK;
    }
    auto read(ch_msg& message) noexcept -> status_t {
        message = {};
        if (ended_)
            return status_;
        for (;;) {
            const auto received = ch_recv(channel_, message, false);
            if (received.status == STATUS_OK) {
                sequence_ = received.value;
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
            const auto armed = sys::channel_arm(channel_, readable_, sequence_);
            if (armed.status != STATUS_OK)
                return armed.status;
            sequence_ = armed.value;
            const auto wake = sys::notification_wait(events_);
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
            ch_msg message{};
            message.size = size < sizeof(message.data) ? size : sizeof(message.data);
            ::memcpy(message.data, text, message.size);
            if (const auto rc = ch_send(output_, message).status; rc != STATUS_OK)
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
inline constexpr rt_req con_output{"console.output", 0x434f4e53, ObjChannel, 2, 1};
inline constexpr rt_req con_input{"console.input", 0x434f4e53, ObjChannel, 2};
inline constexpr rt_req rt_stdin{"stdin", 0x5354524d, ObjChannel, 2};
inline constexpr rt_req rt_stdout{"stdout", 0x5354524d, ObjChannel, 2};
inline constexpr rt_req rt_stderr{"stderr", 0x5354524d, ObjChannel, 2};
enum class con_operation : uint64_t { Bytes, Prompt };
