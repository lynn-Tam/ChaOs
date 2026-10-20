#pragma once
#include <ubus/ubus.hpp>

// A local bounded byte chunk; its representation never crosses the Channel.
// Empty records carry ordered end-of-stream and the producer's final status.
// Status lives in the common envelope; 80 data bytes still fit a native message.
struct str_chunk { status_t status{}; size_t size{}; char data[80]{}; };
inline auto str_message(const str_chunk& chunk) -> ubus_msg {
    ubus_msg message; message.status = chunk.status;
    if (chunk.size) message.data.put("data", std::as_bytes(std::span{chunk.data, chunk.size}));
    return message;
}
inline auto str_decode(const ubus_msg& message, str_chunk& chunk) -> status_t {
    chunk = {}; chunk.status = message.status;
    if (message.count || message.data.view().type() != ubus_type::Record) return STATUS_PEER_FAULT;
    const auto data = message.data["data"];
    if (!data && message.data.bytes().size() == sizeof(ubus_head)) return STATUS_OK;
    const auto bytes = data.as<std::span<const std::byte>>();
    if (!bytes || bytes->empty() || bytes->size() > sizeof(chunk.data)) return STATUS_PEER_FAULT;
    chunk.size = bytes->size(); ::memcpy(chunk.data, bytes->data(), chunk.size);
    return STATUS_OK;
}
inline auto str_send(cap_t channel, const str_chunk& chunk, bool block = true) -> status_t {
    if (chunk.size > sizeof(chunk.data)) return STATUS_BAD_ARGS;
    auto message = str_message(chunk);
    return ubus_send(channel, message, block);
}
inline auto str_receive(ubus_rx& input, str_chunk& chunk) -> status_t {
    ubus_msg message;
    const auto status = input.receive(message);
    return status == STATUS_OK ? str_decode(message, chunk) : status;
}
class str_reader {
    ubus_rx input_;
    cap_t events_{};
    bool ended_{};
    status_t status_{};
public:
    explicit operator bool() const { return bool(input_); }
    auto close() -> status_t {
        const auto status = input_.close();
        if (status == STATUS_OK) { events_ = 0; ended_ = false; status_ = STATUS_OK; }
        return status;
    }
    auto open(cap_t channel, cap_t events) -> status_t {
        const auto status = input_.open(channel, events);
        if (status == STATUS_OK) { events_ = events; ended_ = false; status_ = STATUS_OK; }
        return status;
    }
    auto read(str_chunk& chunk) -> status_t {
        chunk = {};
        if (ended_) return status_;
        for (;;) {
            const auto status = str_receive(input_, chunk);
            if (status == STATUS_OK) {
                if (!chunk.size) { ended_ = true; status_ = chunk.status; }
                return chunk.status;
            }
            if (status != STATUS_WOULD_BLOCK) return status;
            const auto wake = ev_wait(events_);
            if (wake.status != STATUS_OK) return wake.status;
        }
    }
};
class str_writer {
    cap_t output_;
public:
    explicit str_writer(cap_t output) : output_(output) {}
    void write(const char* text, size_t size) const {
        while (size) {
            str_chunk chunk; chunk.size = std::min(size, sizeof(chunk.data));
            ::memcpy(chunk.data, text, chunk.size);
            if (const auto status = str_send(output_, chunk); status != STATUS_OK) sys::exit(status);
            text += chunk.size; size -= chunk.size;
        }
    }
    void write(const char* text) const { write(text, ::strlen(text)); }
    void put(char value) const { write(&value, 1); }
};

inline constexpr rt_key rt_stdin{"stdin", ObjChannel};
inline constexpr rt_key rt_stdout{"stdout", ObjChannel};
inline constexpr rt_key rt_stderr{"stderr", ObjChannel};
