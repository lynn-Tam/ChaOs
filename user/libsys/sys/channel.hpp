#pragma once

#include <stdint.h>
#include <sys/handle.hpp>

namespace sys::service {

inline constexpr uint64_t EventsBadge = 2;

struct Message final {
    uint64_t operation{};
    uint64_t id{};
    int64_t status{};
    uint64_t size{};
    char data[96]{};
};

} // namespace sys::service

namespace sys::service {
// Native services share a layout, not an address space. Each execution's
// declared IPC Mem occupies this address in its own VSpace.
inline constexpr uintptr_t IpcAddress = 0x40000000;

static_assert(sizeof(Message) == CHANNEL_MAX_WORDS * sizeof(word_t));

inline auto length(const char* text) noexcept -> size_t {
    size_t size = 0;
    while (text[size] != '\0') ++size;
    return size;
}
inline auto equal(const char* a, const char* b) noexcept -> bool {
    while (*a != '\0' && *a == *b) { ++a; ++b; }
    return *a == *b;
}
inline void copy(void* to, const void* from, size_t size) noexcept {
    auto* destination = static_cast<uint8_t*>(to);
    const auto* source = static_cast<const uint8_t*>(from);
    for (size_t i = 0; i < size; ++i) destination[i] = source[i];
}

inline auto send(cap_t channel, const Message& message, bool block = true) noexcept -> SysResult {
    auto& wire = *reinterpret_cast<ChanMsg*>(IpcAddress);
    wire = {};
    wire.version = CHANNEL_VERSION;
    wire.word_count = CHANNEL_MAX_WORDS;
    copy(wire.words, &message, sizeof(message));
    return block ? channel_send(channel) : channel_try_send(channel);
}
inline auto send_cap(cap_t channel, const Message& message,
    cap_t capability, word_t rights) noexcept -> SysResult {
    auto& wire = *reinterpret_cast<ChanMsg*>(IpcAddress);
    wire = {};
    wire.version = CHANNEL_VERSION;
    wire.word_count = CHANNEL_MAX_WORDS;
    wire.cap_count = 1;
    wire.caps[0] = {capability, rights, CAP_COPY, 0};
    copy(wire.words, &message, sizeof(message));
    return channel_send(channel);
}
inline auto receive(cap_t channel, Message& message, bool block = true) noexcept -> SysResult {
    auto& wire = *reinterpret_cast<ChanMsg*>(IpcAddress);
    wire = {};
    wire.version = CHANNEL_VERSION;
    auto result = block ? channel_recv(channel) : channel_try_recv(channel);
    if (result.status == STATUS_OK) {
        if (wire.word_count != CHANNEL_MAX_WORDS || wire.received_count != 0)
            return SysResult{.status = STATUS_BAD_ARGS};
        copy(&message, wire.words, sizeof(message));
        if (message.size > sizeof(message.data))
            return SysResult{.status = STATUS_BAD_ARGS};
    }
    return result;
}
inline auto receive_cap(cap_t channel, Message& message,
    cap::OwnedCap& capability) noexcept -> SysResult {
    auto& wire = *reinterpret_cast<ChanMsg*>(IpcAddress);
    wire = {};
    wire.version = CHANNEL_VERSION;
    wire.receive_limit = 1;
    const auto result = channel_try_recv(channel);
    if (result.status != STATUS_OK) return result;
    if (wire.received_count == 1)
        capability = cap::OwnedCap{{wire.received[0], 0}};
    if (wire.word_count != CHANNEL_MAX_WORDS || wire.received_count != 1)
        return {.status = STATUS_BAD_ARGS, .value = result.value};
    copy(&message, wire.words, sizeof(message));
    return message.size <= sizeof(message.data)
        ? result : SysResult{.status = STATUS_BAD_ARGS, .value = result.value};
}
} // namespace sys::service

namespace sys::stream {
// Data and End share the ordinary bounded Channel frame. End is ordered after
// all accepted bytes; its status is the producer's final outcome.
enum class Frame : uint64_t { Data, End };

class Reader final {
    cap_t channel_{}, events_{};
    word_t readable_{};
    uint64_t sequence_{};
    bool ended_{};
    status_t status_{};
public:
    auto open(cap_t channel, cap_t events) noexcept -> status_t {
        const auto result = channel_bind(channel, events, CHANNEL_READABLE);
        if (result.status != STATUS_OK) return result.status;
        channel_ = channel; events_ = events; readable_ = result.value;
        return STATUS_OK;
    }
    auto read(service::Message& message) noexcept -> status_t {
        message = {};
        if (ended_) return status_;
        for (;;) {
            const auto received = service::receive(channel_, message, false);
            if (received.status == STATUS_OK) {
                sequence_ = received.value;
                if (message.operation == static_cast<uint64_t>(Frame::End)) {
                    if (message.size != 0) return STATUS_PEER_FAULT;
                    ended_ = true;
                    status_ = static_cast<status_t>(message.status);
                    return status_;
                }
                if (message.operation != static_cast<uint64_t>(Frame::Data) || message.size == 0)
                    return STATUS_PEER_FAULT;
                return STATUS_OK;
            }
            if (received.status != STATUS_WOULD_BLOCK) return received.status;
            const auto armed = channel_arm(channel_, readable_, sequence_);
            if (armed.status != STATUS_OK) return armed.status;
            sequence_ = armed.value;
            const auto wake = notification_wait(events_);
            if (wake.status != STATUS_OK) return wake.status;
        }
    }
};
} // namespace sys::stream
