#pragma once
#include <string.h>

#include <stdint.h>
#include <sys/handle.hpp>

namespace sys {

struct Message final {
    uint64_t operation{};
    uint64_t id{};
    int64_t status{};
    uint64_t size{};
    char data[96]{};
};


// Native services share a layout, not an address space. Each execution's
// declared IPC Mem occupies this address in its own VSpace.
inline constexpr uintptr_t IpcAddress = 0x40000000;

static_assert(sizeof(Message) == CHANNEL_MAX_WORDS * sizeof(word_t));

inline auto send(cap_t channel, const Message& message, bool block = true,
                 std::span<const CapXfer> caps = {}) noexcept -> SysResult {
    if (caps.size() > CHANNEL_MAX_CAPS) return {.status = STATUS_BAD_ARGS};
    auto& wire = *reinterpret_cast<ChanMsg*>(IpcAddress);
    wire = {};
    wire.version = CHANNEL_VERSION;
    wire.word_count = CHANNEL_MAX_WORDS;
    wire.cap_count = caps.size();
    std::copy(caps.begin(), caps.end(), wire.caps);
    ::memcpy(wire.words, &message, sizeof(message));
    return block ? channel_send(channel) : channel_try_send(channel);
}
inline auto send_cap(cap_t channel, const Message& message,
    cap_t capability, word_t rights) noexcept -> SysResult {
    const CapXfer cap{capability, rights, CAP_COPY, 0};
    return send(channel, message, true, {&cap, 1});
}
inline auto receive(cap_t channel, Message& message, bool block = true,
                    std::span<cap_owner> caps = {}, size_t* count = nullptr) noexcept -> SysResult {
    if (caps.size() > CHANNEL_MAX_CAPS) return {.status = STATUS_BAD_ARGS};
    auto& wire = *reinterpret_cast<ChanMsg*>(IpcAddress);
    wire = {};
    wire.version = CHANNEL_VERSION;
    wire.receive_limit = caps.size();
    auto result = block ? channel_recv(channel) : channel_try_recv(channel);
    if (result.status != STATUS_OK) return result;
    for (size_t i = 0; i < wire.received_count; ++i) caps[i] = cap_owner{wire.received[i]};
    if (count) *count = wire.received_count;
    if (wire.word_count != CHANNEL_MAX_WORDS || (!count && wire.received_count != caps.size()))
        return {.status = STATUS_BAD_ARGS, .value = result.value};
    ::memcpy(&message, wire.words, sizeof(message));
    return message.size <= sizeof(message.data)
        ? result : SysResult{.status = STATUS_BAD_ARGS, .value = result.value};
}
inline auto receive_cap(cap_t channel, Message& message,
    cap_owner& capability) noexcept -> SysResult {
    return receive(channel, message, false, {&capability, 1});
}
} // namespace sys

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
    [[nodiscard]] explicit operator bool() const noexcept { return channel_ != 0; }
    auto close() noexcept -> status_t {
        if (!channel_) return STATUS_OK;
        const auto status = channel_unbind(channel_, readable_).status;
        if (status == STATUS_OK) *this = {};
        return status;
    }
    auto open(cap_t channel, cap_t events) noexcept -> status_t {
        const auto result = channel_bind(channel, events, CHANNEL_READABLE);
        if (result.status != STATUS_OK) return result.status;
        channel_ = channel; events_ = events; readable_ = result.value;
        return STATUS_OK;
    }
    auto read(sys::Message& message) noexcept -> status_t {
        message = {};
        if (ended_) return status_;
        for (;;) {
            const auto received = sys::receive(channel_, message, false);
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

namespace sys {
// A service connection has one reader. Readiness belongs to that reader's
// Notification, allowing one event loop to multiplex several sources.
// Construct once for the service lifetime:
// task authority revocation withdraws the binding. The supervisor stops the
// connected services together if either persistent peer terminates.
class Connection final {
    cap_t channel_;
    cap_t events_;
    word_t readable_;
    uint64_t sequence_{};
    word_t writable_{};
    uint64_t write_sequence_{};
public:
    Connection(cap_t channel, cap_t events) noexcept
        : channel_(channel), events_(events) {
        const auto binding = channel_bind(channel_, events_, CHANNEL_READABLE);
        require(binding.status);
        readable_ = binding.value;
    }
    Connection(const Connection&) = delete;
    auto operator=(const Connection&) -> Connection& = delete;

    auto send(const Message& message, std::span<const CapXfer> caps = {}) const noexcept -> SysResult {
        return sys::send(channel_, message, true, caps);
    }
    auto enable_writable() noexcept -> status_t {
        const auto binding = channel_bind(channel_, events_, CHANNEL_WRITABLE);
        if (binding.status == STATUS_OK) writable_ = binding.value;
        return binding.status;
    }
    auto try_send(const Message& message) noexcept -> SysResult {
        const auto result = sys::send(channel_, message, false);
        if (result.status == STATUS_OK)
            write_sequence_ = result.value;
        return result;
    }
    auto arm_writable() noexcept -> SysResult {
        const auto result = channel_arm(channel_, writable_, write_sequence_);
        if (result.status == STATUS_OK) write_sequence_ = result.value;
        return result;
    }
    auto try_receive(Message& message) noexcept -> SysResult {
        const auto result = sys::receive(channel_, message, false);
        if (result.status == STATUS_OK) sequence_ = result.value;
        return result;
    }
    auto try_receive(Message& message, std::span<cap_owner> caps, size_t& count) noexcept -> SysResult {
        const auto result = sys::receive(channel_, message, false, caps, &count);
        if (result.status == STATUS_OK || result.status == STATUS_BAD_ARGS) sequence_ = result.value;
        return result;
    }
    auto try_receive(Message& message, cap_owner& capability) noexcept -> SysResult {
        const auto result = sys::receive_cap(channel_, message, capability);
        if (result.status == STATUS_OK || result.status == STATUS_BAD_ARGS)
            sequence_ = result.value;
        return result;
    }
    auto arm() noexcept -> SysResult {
        const auto result = channel_arm(channel_, readable_, sequence_);
        if (result.status == STATUS_OK) sequence_ = result.value;
        return result;
    }
    auto receive(Message& message) noexcept -> SysResult {
        for (;;) {
            const auto result = try_receive(message);
            if (result.status == STATUS_OK) {
                return result;
            }
            if (result.status != STATUS_WOULD_BLOCK) return result;
            const auto armed = arm();
            if (armed.status != STATUS_OK) return armed;
            const auto wake = notification_wait(events_);
            if (wake.status != STATUS_OK) return wake;
            // A shared event badge is only a hint. Always recheck this queue;
            // arm compares its sequence atomically with current readiness.
        }
    }
};


} // namespace sys

namespace sys::stream {
// The channel supplies bounded backpressure. No reply queue is shared by
// writers; console access grants only the ability to enqueue output.
class Writer final {
    cap_t output_;
public:
    explicit Writer(cap_t output) noexcept : output_(output) {}
    void write(const char* text, size_t size) const noexcept {
        while (size != 0) {
            sys::Message message{};
            message.size = size < sizeof(message.data) ? size : sizeof(message.data);
            ::memcpy(message.data, text, message.size);
            sys::require(sys::send(output_, message).status);
            text += message.size;
            size -= message.size;
        }
    }
    void write(const char* text) const noexcept { write(text, ::strlen(text)); }
    void put(char value) const noexcept { write(&value, 1); }
};


} // namespace sys::stream
