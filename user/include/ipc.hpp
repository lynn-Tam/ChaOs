#pragma once
#include <string.h>

#include <stdint.h>
#include <array>
#include <util.hpp>
#include <cap.hpp>

struct ch_msg final {
    uint64_t operation{};
    uint64_t id{};
    int64_t status{};
    uint64_t value{};
    uint64_t size{};
    char data[88]{};
};

// Native services share a layout, not an address space. Each execution's
// declared IPC Mem occupies this address in its own VSpace.

static_assert(sizeof(ch_msg) == CHANNEL_MAX_WORDS * sizeof(word_t));

inline auto ch_send(cap_t channel, const ch_msg& message, bool block = true,
                    std::span<const CapXfer> caps = {}) noexcept -> sys::SysResult {
    if (caps.size() > CHANNEL_MAX_CAPS || message.size > sizeof(message.data))
        return {.status = STATUS_BAD_ARGS};
    auto& wire = *reinterpret_cast<ChanMsg*>(rt_startup().ipc);
    wire = {};
    wire.version = CHANNEL_VERSION;
    wire.word_count = CHANNEL_MAX_WORDS;
    wire.cap_count = caps.size();
    std::copy(caps.begin(), caps.end(), wire.caps);
    ::memcpy(wire.words, &message, sizeof(message));
    return block ? sys::channel_send(channel) : sys::channel_try_send(channel);
}
inline auto ch_send_cap(cap_t channel, const ch_msg& message, cap_t endpoint,
                        word_t rights) noexcept -> sys::SysResult {
    const CapXfer cap{endpoint, rights, CAP_COPY, 0};
    return ch_send(channel, message, true, {&cap, 1});
}
inline auto ch_recv(cap_t channel, ch_msg& message, bool block = true,
                    std::span<cap_owner> caps = {}, size_t* count = nullptr,
                    word_t* badge = nullptr) noexcept -> sys::SysResult {
    if (caps.size() > CHANNEL_MAX_CAPS)
        return {.status = STATUS_BAD_ARGS};
    auto& wire = *reinterpret_cast<ChanMsg*>(rt_startup().ipc);
    wire = {};
    wire.version = CHANNEL_VERSION;
    wire.receive_limit = caps.size();
    auto result = block ? sys::channel_recv(channel) : sys::channel_try_recv(channel);
    if (result.status != STATUS_OK)
        return result;
    for (size_t i = 0; i < wire.received_count; ++i)
        caps[i] = cap_owner{wire.received[i]};
    if (count)
        *count = wire.received_count;
    if (badge)
        *badge = wire.sender_badge;
    if (wire.word_count != CHANNEL_MAX_WORDS || (!count && wire.received_count != caps.size()))
        return {.status = STATUS_BAD_ARGS, .value = result.value};
    ::memcpy(&message, wire.words, sizeof(message));
    return message.size <= sizeof(message.data)
               ? result
               : sys::SysResult{.status = STATUS_BAD_ARGS, .value = result.value};
}
inline auto ch_recv_cap(cap_t channel, ch_msg& message, cap_owner& cap) noexcept -> sys::SysResult {
    return ch_recv(channel, message, false, {&cap, 1});
}

// A packet owns all committed transfers, including malformed messages.
struct ch_packet final {
    ch_msg message{};
    std::array<cap_owner, CHANNEL_MAX_CAPS> capabilities{};
    size_t count{};
    word_t badge{};
};
struct ch_reply final {
    ch_msg message{};
    std::array<cap_owner, CHANNEL_MAX_CAPS> capabilities{};
    std::array<word_t, CHANNEL_MAX_CAPS> rights{};
    size_t count{};
    auto offer(cap_owner&& cap, word_t perms) noexcept -> bool {
        if (!cap || count == capabilities.size())
            return false;
        rights[count] = perms;
        capabilities[count++] = std::move(cap);
        return true;
    }
    auto send(cap_t endpoint) const noexcept -> status_t {
        std::array<CapXfer, CHANNEL_MAX_CAPS> caps{};
        for (size_t i = 0; i < count; ++i)
            caps[i] = {capabilities[i].selector(), rights[i], CAP_COPY, 0};
        return ch_send(endpoint, message, false, std::span{caps}.first(count)).status;
    }
    void respond(cap_t endpoint) const noexcept {
        (void) send(endpoint);
        (void) sys::channel_close(endpoint);
    }
};

// A borrowed channel with one reader. End a binding before its cap slot closes.
class ch_port final {
    cap_t channel_{}, events_{};
    word_t readable_{}, writable_{};
    uint64_t sequence_{}, write_sequence_{};

  public:
    ch_port() noexcept = default;
    ch_port(cap_t channel, cap_t events) noexcept {
        if (const auto rc = open(channel, events); rc != STATUS_OK)
            sys::exit(rc);
    }
    ~ch_port() {
        if (const auto rc = close(); rc != STATUS_OK)
            sys::exit(rc);
    }
    ch_port(const ch_port&) = delete;
    auto operator=(const ch_port&) -> ch_port& = delete;
    auto close() noexcept -> status_t {
        for (auto* relation : {&readable_, &writable_})
            if (*relation) {
                const auto s = sys::channel_unbind(channel_, *relation).status;
                // A retired object has already withdrawn all of its relations.
                if (s != STATUS_OK && s != STATUS_BUSY)
                    return s;
                *relation = 0;
            }
        channel_ = events_ = 0;
        return STATUS_OK;
    }
    auto open(cap_t channel, cap_t events) noexcept -> status_t {
        const auto s = close();
        if (s != STATUS_OK)
            return s;
        const auto r = sys::channel_bind(channel, events, CHANNEL_READABLE);
        if (r.status != STATUS_OK)
            return r.status;
        channel_ = channel;
        events_ = events;
        readable_ = r.value;
        sequence_ = write_sequence_ = 0;
        return STATUS_OK;
    }
    auto send(const ch_msg& msg, std::span<const CapXfer> caps = {},
              bool block = true) const noexcept -> sys::SysResult {
        return ch_send(channel_, msg, block, caps);
    }
    auto send(const ch_reply& reply) const noexcept -> status_t { return reply.send(channel_); }
    static auto send_to(cap_t channel, const ch_msg& msg, const CapXfer* caps = nullptr,
                        size_t count = 0) noexcept -> status_t {
        return ch_send(channel, msg, false, {caps, count}).status;
    }
    auto enable_writable() noexcept -> status_t {
        const auto r = sys::channel_bind(channel_, events_, CHANNEL_WRITABLE);
        if (r.status == STATUS_OK)
            writable_ = r.value;
        return r.status;
    }
    auto try_send(const ch_msg& msg) noexcept -> sys::SysResult {
        const auto r = ch_send(channel_, msg, false);
        if (r.status == STATUS_OK)
            write_sequence_ = r.value;
        return r;
    }
    auto arm_writable() noexcept -> sys::SysResult {
        const auto r = sys::channel_arm(channel_, writable_, write_sequence_);
        if (r.status == STATUS_OK)
            write_sequence_ = r.value;
        return r;
    }
    auto try_receive(ch_msg& msg, std::span<cap_owner> caps = {}, size_t* count = nullptr,
                     word_t* badge = nullptr) noexcept -> sys::SysResult {
        const auto r = ch_recv(channel_, msg, false, caps, count, badge);
        if (r.status == STATUS_OK || r.status == STATUS_BAD_ARGS)
            sequence_ = r.value;
        return r;
    }
    auto try_receive(ch_msg& msg, std::span<cap_owner> caps, size_t& count) noexcept
        -> sys::SysResult {
        return try_receive(msg, caps, &count);
    }
    auto try_receive(ch_msg& msg, cap_owner& cap) noexcept -> sys::SysResult {
        return try_receive(msg, {&cap, 1});
    }
    auto receive(ch_packet& packet) noexcept -> status_t {
        packet = {};
        return try_receive(packet.message, packet.capabilities, &packet.count, &packet.badge)
            .status;
    }
    auto arm() noexcept -> sys::SysResult {
        const auto r = sys::channel_arm(channel_, readable_, sequence_);
        if (r.status == STATUS_OK)
            sequence_ = r.value;
        return r;
    }
    auto receive(ch_msg& msg) noexcept -> sys::SysResult {
        for (;;) {
            const auto r = try_receive(msg);
            if (r.status != STATUS_WOULD_BLOCK)
                return r;
            const auto armed = arm();
            if (armed.status != STATUS_OK)
                return armed;
            const auto wake = sys::notification_wait(events_);
            if (wake.status != STATUS_OK)
                return wake;
        }
    }
};

// Own the channel object, not merely its local CSpace slots.
class ch_pair final {
  public:
    std::array<cap_owner, 2> ends;
    ch_pair() noexcept = default;
    ch_pair(const ch_pair&) = delete;
    auto operator=(const ch_pair&) -> ch_pair& = delete;
    ~ch_pair() {
        if (const auto rc = close(); rc != STATUS_OK)
            sys::exit(rc);
    }
    auto close() noexcept -> status_t {
        const auto root = ends[0] ? ends[0].selector() : ends[1].selector();
        if (root) {
            const auto s = sys::object_destroy(root).status;
            if (s != STATUS_OK)
                return s;
        }
        for (auto& end : ends) {
            const auto s = end.close();
            if (s != STATUS_OK)
                return s;
        }
        return STATUS_OK;
    }
    auto open(cap_t pool, word_t depth = 4, word_t transfers = 4, word_t relations = 2) noexcept
        -> status_t {
        const auto s = close();
        if (s != STATUS_OK)
            return s;
        const auto r = sys::channel_create(pool, depth, CHANNEL_MAX_WORDS, transfers, relations);
        ends[0] = cap_owner{r.value};
        ends[1] = cap_owner{r.value2};
        return r.status;
    }
};

// Each call owns a fresh reply receiver and moves its sender to the provider.
// An explicit deadline bounds waiting, not mutation or payload retirement.
class ch_rpc final {
    cap_t directory_{}, pool_{}, cspace_{}, events_{};

  public:
    auto open(cap_t directory, cap_t pool, cap_t cspace, cap_t events) noexcept -> status_t {
        if (!directory || !pool || !cspace || !events)
            return STATUS_BAD_ARGS;
        directory_ = directory;
        pool_ = pool;
        cspace_ = cspace;
        events_ = events;
        return STATUS_OK;
    }
    auto exchange(ch_msg& msg, ch_packet& packet, uint64_t deadline = 0) noexcept -> status_t {
        if (!directory_)
            return STATUS_INVALID_CAP;
        ch_pair reply;
        auto s = reply.open(pool_, 1, CHANNEL_MAX_CAPS, 1);
        if (s != STATUS_OK)
            return s;
        ch_port port;
        s = port.open(reply.ends[0].selector(), events_);
        const auto minted =
            sys::channel_mint(reply.ends[1].selector(), cspace_, 1, RIGHT_SEND | RIGHT_CLOSE);
        cap_owner sender{minted.value};
        if (minted.status == STATUS_OK)
            reply.ends[1] = {};
        if (s == STATUS_OK)
            s = minted.status;
        const CapXfer endpoint{sender.selector(), 0, CAP_MOVE, 0};
        if (s == STATUS_OK)
            s = ch_send(directory_, msg, deadline == 0, {&endpoint, 1}).status;
        if (s == STATUS_OK)
            (void) sender.release();
        if (s != STATUS_OK)
            return s;
        for (;;) {
            s = port.receive(packet);
            if (s == STATUS_OK) {
                if (packet.message.id != msg.id || packet.message.operation != msg.operation)
                    return STATUS_PEER_FAULT;
                msg = packet.message;
                return msg.status;
            }
            if (s != STATUS_WOULD_BLOCK)
                return s;
            s = port.arm().status;
            if (s == STATUS_OK)
                s = sys::notification_wait(events_, deadline).status;
            if (s != STATUS_OK)
                return s;
        }
    }
    auto exchange(ch_msg& msg, uint64_t deadline = 0) noexcept -> status_t {
        ch_packet packet;
        const auto s = exchange(msg, packet, deadline);
        return packet.count ? STATUS_PEER_FAULT : s;
    }
};
