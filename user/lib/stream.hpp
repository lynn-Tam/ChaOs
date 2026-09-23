#pragma once

#include <user/lib/service.hpp>

namespace myos::stream {
// Data and End share the ordinary bounded Channel frame. End is ordered after
// all accepted bytes; its status is the producer's final outcome.
enum class Frame : uint64_t { Data, End };

// The channel supplies bounded backpressure. No reply queue is shared by
// writers; console access grants only the ability to enqueue output.
class Writer final {
    myos_cap_t output_;
public:
    explicit Writer(myos_cap_t output) noexcept : output_(output) {}
    void write(const char* text, size_t size) const noexcept {
        while (size != 0) {
            service::Message message{};
            message.size = size < sizeof(message.data) ? size : sizeof(message.data);
            service::copy(message.data, text, message.size);
            service::require(service::send(output_, message).status);
            text += message.size;
            size -= message.size;
        }
    }
    void write(const char* text) const noexcept { write(text, service::length(text)); }
    void put(char value) const noexcept { write(&value, 1); }
};


class Reader final {
    myos_cap_t channel_{}, events_{};
    myos_word_t readable_{};
    uint64_t sequence_{};
    bool ended_{};
    myos_status_t status_{};
public:
    auto open(myos_cap_t channel, myos_cap_t events) noexcept -> myos_status_t {
        const auto result = channel_bind(channel, events, MYOS_CHANNEL_READABLE);
        if (result.status != MYOS_STATUS_OK) return result.status;
        channel_ = channel; events_ = events; readable_ = result.value;
        return MYOS_STATUS_OK;
    }
    auto read(service::Message& message) noexcept -> myos_status_t {
        message = {};
        if (ended_) return status_;
        for (;;) {
            const auto received = service::receive(channel_, message, false);
            if (received.status == MYOS_STATUS_OK) {
                sequence_ = received.value;
                if (message.operation == static_cast<uint64_t>(Frame::End)) {
                    if (message.size != 0) return MYOS_STATUS_PEER_FAULT;
                    ended_ = true;
                    status_ = static_cast<myos_status_t>(message.status);
                    return status_;
                }
                if (message.operation != static_cast<uint64_t>(Frame::Data) || message.size == 0)
                    return MYOS_STATUS_PEER_FAULT;
                return MYOS_STATUS_OK;
            }
            if (received.status != MYOS_STATUS_WOULD_BLOCK) return received.status;
            const auto armed = channel_arm(channel_, readable_, sequence_);
            if (armed.status != MYOS_STATUS_OK) return armed.status;
            sequence_ = armed.value;
            const auto wake = notification_wait(events_);
            if (wake.status != MYOS_STATUS_OK) return wake.status;
        }
    }
};
} // namespace myos::stream
