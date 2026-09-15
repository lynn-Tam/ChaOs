#pragma once

#include <stddef.h>
#include <stdint.h>
#include <user/lib/bootstrap.hpp>
#include <user/lib/syscall.hpp>
#include <user/lib/service_protocol.hpp>

namespace myos::service {

// Native services share a layout, not an address space. Each execution's
// declared IPC MemoryObject occupies this address in its own VSpace.
inline constexpr uintptr_t IpcAddress = 0x40000000;

static_assert(sizeof(Message) == MYOS_CHANNEL_MAX_WORDS * sizeof(myos_word_t));

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

inline auto send(myos_cap_t channel, const Message& message, bool block = true) noexcept -> SysResult {
    auto& wire = *reinterpret_cast<myos_channel_message*>(IpcAddress);
    wire = {};
    wire.version = MYOS_CHANNEL_VERSION;
    wire.word_count = MYOS_CHANNEL_MAX_WORDS;
    copy(wire.words, &message, sizeof(message));
    return block ? channel_send(channel) : channel_try_send(channel);
}
inline auto receive(myos_cap_t channel, Message& message, bool block = true) noexcept -> SysResult {
    auto& wire = *reinterpret_cast<myos_channel_message*>(IpcAddress);
    wire = {};
    wire.version = MYOS_CHANNEL_VERSION;
    auto result = block ? channel_recv(channel) : channel_try_recv(channel);
    if (result.status == MYOS_STATUS_OK) {
        if (wire.word_count != MYOS_CHANNEL_MAX_WORDS || wire.received_count != 0)
            return SysResult{.status = MYOS_STATUS_BAD_ARGS};
        copy(&message, wire.words, sizeof(message));
        if (message.size > sizeof(message.data))
            return SysResult{.status = MYOS_STATUS_BAD_ARGS};
    }
    return result;
}
inline void require(myos_status_t status) noexcept {
    if (status != MYOS_STATUS_OK) myos::exit(status);
}
inline auto bootstrap(const void* address, myos_word_t size) noexcept -> bootstrap::BootstrapView {
    auto result = bootstrap::BootstrapView::parse(address, size);
    if (!result || result->cpu_count() == 0) myos::exit(MYOS_STATUS_BAD_ARGS);
    return *result;
}
template<class Binding>
inline auto capability(const bootstrap::BootstrapView& info, Binding role) noexcept -> myos_cap_t {
    auto cap = info.selector(role);
    if (cap == 0) myos::exit(MYOS_STATUS_INVALID_CAP);
    return cap;
}

// A service connection has one reader. Readiness belongs to that reader's
// Notification, so opposite channel sides never compete for the channel's
// single blocking-operation slot. Construct once for the service lifetime:
// task authority revocation withdraws the binding. The supervisor stops the
// connected services together if either persistent peer terminates.
class Connection final {
    myos_cap_t channel_;
    myos_cap_t events_;
    myos_word_t readable_;
    uint64_t sequence_{};
    myos_word_t writable_{};
    uint64_t write_sequence_{};
public:
    Connection(myos_cap_t channel, myos_cap_t events) noexcept
        : channel_(channel), events_(events) {
        const auto binding = channel_bind(channel_, events_, MYOS_CHANNEL_READABLE);
        require(binding.status);
        readable_ = binding.value;
    }
    Connection(const Connection&) = delete;
    auto operator=(const Connection&) -> Connection& = delete;

    auto send(const Message& message) const noexcept -> SysResult {
        return service::send(channel_, message);
    }
    auto enable_writable() noexcept -> myos_status_t {
        const auto binding = channel_bind(channel_, events_, MYOS_CHANNEL_WRITABLE);
        if (binding.status == MYOS_STATUS_OK) writable_ = binding.value;
        return binding.status;
    }
    auto try_send(const Message& message) noexcept -> SysResult {
        const auto result = service::send(channel_, message, false);
        if (result.status == MYOS_STATUS_OK || result.status == MYOS_STATUS_WOULD_BLOCK)
            write_sequence_ = result.value;
        return result;
    }
    auto arm_writable() noexcept -> SysResult {
        return channel_arm(channel_, writable_, write_sequence_);
    }
    auto try_receive(Message& message) noexcept -> SysResult {
        const auto result = service::receive(channel_, message, false);
        if (result.status == MYOS_STATUS_OK) sequence_ = result.value;
        return result;
    }
    auto arm() noexcept -> SysResult { return channel_arm(channel_, readable_, sequence_); }
    auto receive(Message& message) noexcept -> SysResult {
        for (;;) {
            const auto result = try_receive(message);
            if (result.status == MYOS_STATUS_OK) {
                return result;
            }
            if (result.status != MYOS_STATUS_WOULD_BLOCK) return result;
            const auto armed = arm();
            if (armed.status != MYOS_STATUS_OK) return armed;
            const auto wake = notification_wait(events_);
            if (wake.status != MYOS_STATUS_OK) return wake;
            // A shared event badge is only a hint. Always recheck this queue;
            // arm compares its sequence atomically with current readiness.
        }
    }
};

// The channel supplies bounded backpressure. No reply queue is shared by
// writers; console access grants only the ability to enqueue output.
class Console final {
    myos_cap_t output_;
public:
    explicit Console(myos_cap_t output) noexcept : output_(output) {}
    void write(const char* text, size_t size) const noexcept {
        while (size != 0) {
            Message message{};
            message.size = size < sizeof(message.data) ? size : sizeof(message.data);
            copy(message.data, text, message.size);
            for (;;) {
                const auto status = send(output_, message).status;
                if (status == MYOS_STATUS_BUSY || status == MYOS_STATUS_RETRY) {
                    myos::yield();
                    continue;
                }
                require(status);
                break;
            }
            text += message.size;
            size -= message.size;
        }
    }
    void write(const char* text) const noexcept { write(text, length(text)); }
    void put(char value) const noexcept { write(&value, 1); }
};

} // namespace myos::service
