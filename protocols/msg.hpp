#pragma once

#include <ipc.hpp>
#include <util.hpp>
#include <bit>

struct msg_frame final {
    uint64_t operation{};
    int64_t status{};
    uint64_t value{};
    uint64_t size{};
    uint64_t deadline{};
    char data[80]{};
};

// One shared service envelope; one-way streams do not use the reply client.
inline constexpr size_t msg_words = sizeof(msg_frame) / sizeof(word_t);
static_assert(sizeof(msg_frame) % sizeof(word_t) == 0 && msg_words <= CHANNEL_MAX_WORDS);
inline auto msg_send(cap_t channel, const msg_frame& message, bool block = true,
                     std::span<const CapXfer> caps = {}) noexcept -> sys::SysResult {
    if (message.size > sizeof(message.data)) return {.status = STATUS_BAD_ARGS};
    return ch_send(channel, std::bit_cast<std::array<word_t, msg_words>>(message), block, caps);
}
inline auto msg_recv(cap_t channel, msg_frame& message, bool block = true,
                     std::span<cap_owner> caps = {}, size_t* count = nullptr,
                     word_t* badge = nullptr) noexcept -> sys::SysResult {
    ch_packet packet;
    auto result = ch_recv(channel, packet, block, caps.size());
    if (result.status != STATUS_OK) return result;
    std::move(packet.capabilities.begin(), packet.capabilities.begin() + packet.count, caps.begin());
    if (count) *count = packet.count;
    if (badge) *badge = packet.badge;
    if (packet.word_count != msg_words || (!count && packet.count != caps.size()))
        return {.status = STATUS_BAD_ARGS, .value = result.value};
    std::array<word_t, msg_words> words;
    std::copy_n(packet.words.begin(), msg_words, words.begin());
    message = std::bit_cast<msg_frame>(words);
    return message.size <= sizeof(message.data) ? result
        : sys::SysResult{.status = STATUS_BAD_ARGS, .value = result.value};
}

// A packet owns all committed transfers, including malformed messages.
struct msg_packet final {
    msg_frame message{};
    std::array<cap_owner, CHANNEL_MAX_CAPS> capabilities{};
    size_t count{};
    word_t badge{};
};
struct msg_reply final {
    msg_frame message{};
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
    void respond(cap_t endpoint) const noexcept {
        std::array<CapXfer, CHANNEL_MAX_CAPS> caps{};
        for (size_t i = 0; i < count; ++i)
            caps[i] = {capabilities[i].selector(), rights[i], CAP_COPY, 0};
        (void) msg_send(endpoint, message, false, std::span{caps}.first(count));
        (void) sys::channel_close(endpoint);
    }
};

// A borrowed channel with one reader. End a binding before its cap slot closes.
class msg_rx final {
    cap_t channel_{};
    word_t readable_{};
    uint64_t sequence_{};

  public:
    msg_rx() noexcept = default;
    msg_rx(cap_t channel, cap_t events) noexcept {
        if (const auto rc = open(channel, events); rc != STATUS_OK)
            sys::exit(rc);
    }
    ~msg_rx() {
        if (const auto rc = close(); rc != STATUS_OK)
            sys::exit(rc);
    }
    msg_rx(const msg_rx&) = delete;
    auto operator=(const msg_rx&) -> msg_rx& = delete;
    msg_rx(msg_rx&& other) noexcept { *this = std::move(other); }
    auto operator=(msg_rx&& other) noexcept -> msg_rx& {
        if (this != &other) {
            if (const auto status = close(); status != STATUS_OK) sys::exit(status);
            channel_ = std::exchange(other.channel_, 0);
            readable_ = std::exchange(other.readable_, 0);
            sequence_ = std::exchange(other.sequence_, 0);
        }
        return *this;
    }
    explicit operator bool() const noexcept { return channel_ != 0; }
    auto close() noexcept -> status_t {
        if (readable_) {
            const auto status = sys::channel_unbind(channel_, readable_).status;
            if (status != STATUS_OK && status != STATUS_BUSY)
                return status;
            readable_ = 0;
        }
        channel_ = 0;
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
        readable_ = r.value;
        sequence_ = 0;
        return STATUS_OK;
    }
    auto try_receive(msg_frame& msg, std::span<cap_owner> caps = {}, size_t* count = nullptr,
                     word_t* badge = nullptr) noexcept -> sys::SysResult {
        const auto r = msg_recv(channel_, msg, false, caps, count, badge);
        if (r.status == STATUS_OK || r.status == STATUS_BAD_ARGS)
            sequence_ = r.value;
        return r;
    }
    auto receive(msg_packet& packet) noexcept -> status_t {
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
};

// Each call owns a fresh reply receiver and moves its sender to the provider.
// An explicit deadline bounds waiting, not mutation or payload retirement.
class rpc_client final {
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
    auto exchange(msg_frame& msg, msg_packet& packet, std::span<const CapXfer> caps = {}) noexcept
        -> status_t {
        if (!directory_)
            return STATUS_INVALID_CAP;
        if (caps.size() >= CHANNEL_MAX_CAPS)
            return STATUS_BAD_ARGS;
        const auto deadline = msg.deadline;
        if (const auto status = rt_admit(deadline); status != STATUS_OK)
            return status;
        ch_pair reply;
        auto s = reply.open(pool_, 1, CHANNEL_MAX_CAPS, 1);
        if (s != STATUS_OK)
            return s;
        msg_rx port;
        s = port.open(reply.ends[0].selector(), events_);
        const auto minted =
            sys::channel_mint(reply.ends[1].selector(), cspace_, 1, RIGHT_SEND | RIGHT_CLOSE);
        cap_owner sender{minted.value};
        if (minted.status == STATUS_OK)
            reply.ends[1] = {};
        if (s == STATUS_OK)
            s = minted.status;
        std::array<CapXfer, CHANNEL_MAX_CAPS> transfers{};
        transfers[0] = {sender.selector(), 0, CAP_MOVE, 0};
        std::copy(caps.begin(), caps.end(), transfers.begin() + 1);
        if (s == STATUS_OK)
            s = msg_send(directory_, msg, deadline == 0, std::span{transfers}.first(caps.size() + 1))
                    .status;
        if (s == STATUS_OK)
            (void) sender.release();
        if (s != STATUS_OK)
            return s;
        for (;;) {
            s = port.receive(packet);
            if (s == STATUS_OK) {
                if (packet.message.operation != msg.operation)
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
    auto exchange(msg_frame& msg) noexcept -> status_t {
        msg_packet packet;
        const auto s = exchange(msg, packet);
        return packet.count ? STATUS_PEER_FAULT : s;
    }
};

// A single bounded asynchronous request without capability payloads. The reply
// sender moves exactly once; timeout closes the reply path, not the mutation.
class rpc_pending {
    ch_pair reply_;
    msg_rx input_;
    cap_owner sender_;
    cap_t endpoint_{};
    msg_frame request_{};
    bool sent_{};
public:
    auto active() const noexcept -> bool { return endpoint_ != 0; }
    auto begin(cap_t endpoint, const rt_env& env, msg_frame request) noexcept -> status_t {
        if (active() || !endpoint) return STATUS_BUSY;
        auto status = reply_.open(env.pool, 1, 0, 1);
        if (status == STATUS_OK) status = input_.open(reply_.ends[0].selector(), env.events);
        if (status == STATUS_OK) {
            const auto minted = sys::channel_mint(reply_.ends[1].selector(), env.cspace, 1, RIGHT_SEND | RIGHT_CLOSE);
            sender_ = cap_owner{minted.value}; status = minted.status;
        }
        if (status != STATUS_OK) {
            (void)input_.close(); sender_ = {}; (void)reply_.close(); return status;
        }
        reply_.ends[1] = {};
        endpoint_ = endpoint; request_ = request; sent_ = false;
        return STATUS_OK;
    }
    auto poll(msg_frame& response) noexcept -> status_t {
        if (!active()) return STATUS_INVALID_OP;
        bool completed{};
        status_t status = rt_admit(request_.deadline);
        if (status == STATUS_OK && !sent_) {
            const CapXfer transfer{sender_.selector(), 0, CAP_MOVE, 0};
            status = msg_send(endpoint_, request_, false, {&transfer, 1}).status;
            if (status == STATUS_OK) { (void)sender_.release(); sent_ = true; }
        }
        if (status == STATUS_OK) {
            const auto received = input_.try_receive(response);
            status = received.status;
            if (status == STATUS_OK) {
                completed = true;
                status = response.operation == request_.operation ? static_cast<status_t>(response.status) : STATUS_PEER_FAULT;
            }
            else if (status == STATUS_WOULD_BLOCK) (void)input_.arm();
        }
        if (!completed && (status == STATUS_WOULD_BLOCK || status == STATUS_BUSY)) return STATUS_WOULD_BLOCK;
        endpoint_ = 0;
        const auto closed = input_.close();
        if (closed != STATUS_OK) return closed;
        sender_ = {};
        const auto retired = reply_.close();
        return status == STATUS_OK ? retired : status;
    }
};
