#pragma once
#include <string.h>

#include <stdint.h>
#include <array>
#include <sys/clock.hpp>
#include <sys/handle.hpp>

namespace sys {

struct Message final {
    uint64_t operation{};
    uint64_t id{};
    int64_t status{};
    uint64_t value{};
    uint64_t size{};
    char data[88]{};
};


// Native services share a layout, not an address space. Each execution's
// declared IPC Mem occupies this address in its own VSpace.
inline constexpr uintptr_t IpcAddress = 0x40000000;

static_assert(sizeof(Message) == CHANNEL_MAX_WORDS * sizeof(word_t));

inline auto send(cap_t channel, const Message& message, bool block = true,
                 std::span<const CapXfer> caps = {}) noexcept -> SysResult {
    if (caps.size() > CHANNEL_MAX_CAPS || message.size > sizeof(message.data)) return {.status = STATUS_BAD_ARGS};
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
                    std::span<cap_owner> caps = {}, size_t* count = nullptr, word_t* badge = nullptr) noexcept -> SysResult {
    if (caps.size() > CHANNEL_MAX_CAPS) return {.status = STATUS_BAD_ARGS};
    auto& wire = *reinterpret_cast<ChanMsg*>(IpcAddress);
    wire = {};
    wire.version = CHANNEL_VERSION;
    wire.receive_limit = caps.size();
    auto result = block ? channel_recv(channel) : channel_try_recv(channel);
    if (result.status != STATUS_OK) return result;
    for (size_t i = 0; i < wire.received_count; ++i) caps[i] = cap_owner{wire.received[i]};
    if (count) *count = wire.received_count;
    if (badge) *badge = wire.sender_badge;
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
// A packet owns all committed transfers, including malformed messages.
struct Packet final {
    Message message{};
    std::array<cap_owner, CHANNEL_MAX_CAPS> capabilities{};
    size_t count{};
    word_t badge{};
};
struct Reply final {
    Message message{};
    std::array<cap_owner, CHANNEL_MAX_CAPS> capabilities{};
    std::array<word_t, CHANNEL_MAX_CAPS> rights{};
    size_t count{};
    auto offer(cap_owner&& cap, word_t perms) noexcept -> bool {
        if (!cap || count == capabilities.size()) return false;
        rights[count] = perms;
        capabilities[count++] = std::move(cap);
        return true;
    }
    auto send(cap_t endpoint) const noexcept -> status_t {
        std::array<CapXfer, CHANNEL_MAX_CAPS> caps{};
        for (size_t i = 0; i < count; ++i)
            caps[i] = {capabilities[i].selector(), rights[i], CAP_COPY, 0};
        return sys::send(endpoint, message, false, std::span{caps}.first(count)).status;
    }
    void respond(cap_t endpoint) const noexcept {
        (void)send(endpoint);
        (void)channel_close(endpoint);
    }

};

// A borrowed channel with one reader. End a binding before its cap slot closes.
class Port final {
    cap_t channel_{}, events_{};
    word_t readable_{}, writable_{};
    uint64_t sequence_{}, write_sequence_{};
public:
    Port() noexcept = default;
    Port(cap_t channel, cap_t events) noexcept { require(open(channel, events)); }
    Port(const Port&) = delete;
    auto operator=(const Port&) -> Port& = delete;
    auto close() noexcept -> status_t {
        for (auto* relation : {&readable_, &writable_}) if (*relation) {
            const auto s = channel_unbind(channel_, *relation).status;
            // A retired object has already withdrawn all of its relations.
            if (s != STATUS_OK && s != STATUS_BUSY) return s;
            *relation = 0;
        }
        channel_ = events_ = 0;
        return STATUS_OK;
    }
    auto open(cap_t channel, cap_t events) noexcept -> status_t {
        const auto s = close();
        if (s != STATUS_OK) return s;
        const auto r = channel_bind(channel, events, CHANNEL_READABLE);
        if (r.status != STATUS_OK) return r.status;
        channel_ = channel; events_ = events; readable_ = r.value; sequence_ = write_sequence_ = 0;
        return STATUS_OK;
    }
    auto send(const Message& msg, std::span<const CapXfer> caps = {}, bool block = true) const noexcept -> SysResult {
        return sys::send(channel_, msg, block, caps);
    }
    auto send(const Reply& reply) const noexcept -> status_t { return reply.send(channel_); }
    static auto send_to(cap_t channel, const Message& msg,
                        const CapXfer* caps = nullptr, size_t count = 0) noexcept -> status_t {
        return sys::send(channel, msg, false, {caps, count}).status;
    }
    auto enable_writable() noexcept -> status_t {
        const auto r = channel_bind(channel_, events_, CHANNEL_WRITABLE);
        if (r.status == STATUS_OK) writable_ = r.value;
        return r.status;
    }
    auto try_send(const Message& msg) noexcept -> SysResult {
        const auto r = sys::send(channel_, msg, false);
        if (r.status == STATUS_OK) write_sequence_ = r.value;
        return r;
    }
    auto arm_writable() noexcept -> SysResult {
        const auto r = channel_arm(channel_, writable_, write_sequence_);
        if (r.status == STATUS_OK) write_sequence_ = r.value;
        return r;
    }
    auto try_receive(Message& msg, std::span<cap_owner> caps = {}, size_t* count = nullptr,
                     word_t* badge = nullptr) noexcept -> SysResult {
        const auto r = sys::receive(channel_, msg, false, caps, count, badge);
        if (r.status == STATUS_OK || r.status == STATUS_BAD_ARGS) sequence_ = r.value;
        return r;
    }
    auto try_receive(Message& msg, std::span<cap_owner> caps, size_t& count) noexcept -> SysResult {
        return try_receive(msg, caps, &count);
    }
    auto try_receive(Message& msg, cap_owner& cap) noexcept -> SysResult {
        return try_receive(msg, {&cap, 1});
    }
    auto receive(Packet& packet) noexcept -> status_t {
        packet = {};
        return try_receive(packet.message, packet.capabilities, &packet.count, &packet.badge).status;
    }
    auto arm() noexcept -> SysResult {
        const auto r = channel_arm(channel_, readable_, sequence_);
        if (r.status == STATUS_OK) sequence_ = r.value;
        return r;
    }
    auto receive(Message& msg) noexcept -> SysResult {
        for (;;) {
            const auto r = try_receive(msg);
            if (r.status != STATUS_WOULD_BLOCK) return r;
            const auto armed = arm();
            if (armed.status != STATUS_OK) return armed;
            const auto wake = notification_wait(events_);
            if (wake.status != STATUS_OK) return wake;
        }
    }
};

// Own the channel object, not merely its local CSpace slots.
class Channel final {
public:
    std::array<cap_owner, 2> ends;
    Channel() noexcept = default;
    Channel(const Channel&) = delete;
    auto operator=(const Channel&) -> Channel& = delete;
    ~Channel() { require(close()); }
    auto close() noexcept -> status_t {
        const auto root = ends[0] ? ends[0].selector() : ends[1].selector();
        if (root) {
            const auto s = object_destroy(root).status;
            if (s != STATUS_OK) return s;
        }
        for (auto& end : ends) {
            const auto s = end.close();
            if (s != STATUS_OK) return s;
        }
        return STATUS_OK;
    }
    auto open(cap_t pool, word_t depth = 4, word_t transfers = 4, word_t relations = 2) noexcept -> status_t {
        const auto s = close();
        if (s != STATUS_OK) return s;
        const auto r = channel_create(pool, depth, CHANNEL_MAX_WORDS, transfers, relations);
        ends[0] = cap_owner{r.value}; ends[1] = cap_owner{r.value2};
        return r.status;
    }
};

// Each call has a fresh reply object and one bounded wait. Timeout means the
// outcome is unknown; never retry a mutating request or reuse its reply queue.
class Rpc final {
    cap_t directory_{}, pool_{}, cspace_{}, events_{};
    Clock clock_;
public:
    auto open(cap_t directory, cap_t pool, cap_t cspace, cap_t events) noexcept -> status_t {
        if (!directory || !pool || !cspace || !events) return STATUS_BAD_ARGS;
        const auto s = clock_.open();
        if (s == STATUS_OK) { directory_ = directory; pool_ = pool; cspace_ = cspace; events_ = events; }
        return s;
    }
    auto exchange(Message& msg, Packet& packet) noexcept -> status_t {
        if (!directory_) return STATUS_INVALID_CAP;
        const auto deadline = clock_.after_ms(5'000);
        if (!deadline) return STATUS_INTERNAL;
        Channel reply;
        auto s = reply.open(pool_, 1, CHANNEL_MAX_CAPS, 1);
        if (s != STATUS_OK) return s;
        Port port;
        s = port.open(reply.ends[0].selector(), events_);
        const auto minted = channel_mint(reply.ends[1].selector(), cspace_, 1,
            RIGHT_SEND | RIGHT_CLOSE | RIGHT_DUPLICATE);
        cap_owner sender{minted.value};
        if (s == STATUS_OK) s = minted.status;
        const CapXfer endpoint{sender.selector(), RIGHT_SEND | RIGHT_CLOSE, CAP_COPY, 0};
        if (s == STATUS_OK) s = sys::send(directory_, msg, false, {&endpoint, 1}).status;
        if (s != STATUS_OK) return s;
        for (;;) {
            s = port.receive(packet);
            if (s == STATUS_OK) {
                if (packet.message.id != msg.id || packet.message.operation != msg.operation)
                    return STATUS_PEER_FAULT;
                msg = packet.message;
                return msg.status;
            }
            if (s != STATUS_WOULD_BLOCK) return s;
            s = port.arm().status;
            if (s == STATUS_OK) s = notification_wait(events_, *deadline).status;
            if (s != STATUS_OK) return s;
        }
    }
    auto exchange(Message& msg) noexcept -> status_t {
        Packet packet;
        const auto s = exchange(msg, packet);
        return packet.count ? STATUS_PEER_FAULT : s;
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
