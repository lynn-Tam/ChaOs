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
    uint64_t sequence{}; // Identifies one invocation on its reply transport.
    char data[80]{};
};

// One shared service envelope; one-way streams do not use the reply client.
inline constexpr size_t msg_words = sizeof(msg_frame) / sizeof(word_t);
static_assert(sizeof(msg_frame) % sizeof(word_t) == 0 && msg_words <= CHANNEL_MAX_WORDS);
inline auto msg_send(cap_t channel, const msg_frame& message, bool block = true,
                     std::span<const CapXfer> caps = {}, cap_t reply = 0) noexcept -> sys::SysResult {
    if (message.size > sizeof(message.data)) return {.status = STATUS_BAD_ARGS};
    return ch_send(channel, std::bit_cast<std::array<word_t, msg_words>>(message), block, caps, 0, reply);
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
        if (!message.sequence) (void) sys::channel_close(endpoint);
    }
};

// A borrowed channel with one reader. End a binding before its cap slot closes.
class msg_rx final {
    cap_t channel_{};
    ev_watch readable_;
    status_t terminal_{};

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
            readable_ = std::move(other.readable_);
            terminal_ = std::exchange(other.terminal_, 0);
        }
        return *this;
    }
    explicit operator bool() const noexcept { return channel_ != 0; }
    auto close() noexcept -> status_t {
        const auto status = readable_.close();
        if (status == STATUS_OK) { channel_ = 0; terminal_ = 0; }
        return status;
    }
    auto open(cap_t channel, cap_t events) noexcept -> status_t {
        if (const auto status = close(); status != STATUS_OK) return status;
        const auto status = events ? readable_.open(events, channel, SOURCE_READY | SOURCE_TERMINAL, 0, 0, SOURCE_READ) : STATUS_OK;
        if (status == STATUS_OK) channel_ = channel;
        return status;
    }
    auto try_receive(msg_frame& msg, std::span<cap_owner> caps = {}, size_t* count = nullptr,
                     word_t* badge = nullptr) noexcept -> sys::SysResult {
        if (terminal_) return {.status=terminal_};
        const auto result = msg_recv(channel_, msg, false, caps, count, badge);
        if (result.status == STATUS_CLOSED || result.status == STATUS_PEER_CLOSED) {
            terminal_ = result.status;
            if (const auto status = readable_.close(); status != STATUS_OK) return {.status=status};
        }
        return result;
    }
    auto receive(msg_packet& packet) noexcept -> status_t {
        packet = {};
        return try_receive(packet.message, packet.capabilities, &packet.count, &packet.badge)
            .status;
    }
    auto arm() noexcept -> sys::SysResult {
        return terminal_ || !readable_ ? sys::SysResult{.status=STATUS_OK} : readable_.arm();
    }
};

// One pending call owns a persistent reply transport. The kernel creates the
// movable, one-shot reply facet; timeout never reuses its generation.
class rpc_pending {
    ch_pair reply_;
    msg_rx input_;
    cap_t endpoint_{}, events_{};
    ev_watch writable_;
    msg_frame request_{};
    std::array<CapXfer, CHANNEL_MAX_CAPS> transfers_{};
    size_t count_{};
    uint64_t sequence_{}, token_{};
    void disarm() noexcept {
        if (const auto status = writable_.close(); status != STATUS_OK) sys::exit(status);
    }
    auto send() noexcept -> status_t {
        const auto sent = msg_send(endpoint_, request_, false, std::span{transfers_}.first(count_), reply_.ends[1].selector());
        if (sent.status == STATUS_OK) { token_ = sent.value; disarm(); return STATUS_OK; }
        if (sent.status != STATUS_WOULD_BLOCK && sent.status != STATUS_BUSY) return sent.status;
        if (!events_) return STATUS_WOULD_BLOCK;
        const auto armed = writable_ ? writable_.arm().status
            : writable_.open(events_, endpoint_, SOURCE_READY | SOURCE_TERMINAL, 0, 0, SOURCE_WRITE);
        return armed == STATUS_OK ? STATUS_WOULD_BLOCK : armed;
    }
public:
    ~rpc_pending() { if (const auto status = close(); status != STATUS_OK) sys::exit(status); }
    auto deadline() const noexcept -> uint64_t { return active() && request_.deadline ? request_.deadline : UINT64_MAX; }
    auto active() const noexcept -> bool { return endpoint_ != 0; }
    auto wait() noexcept -> status_t {
        return sys::wait_one(token_ ? reply_.ends[0].selector() : endpoint_,
            SOURCE_READY | SOURCE_TERMINAL, 0, request_.deadline, 0, token_ ? SOURCE_READ : SOURCE_WRITE).status;
    }
    auto cancel() noexcept -> status_t {
        disarm(); endpoint_ = 0;
        if (!token_) return STATUS_OK;
        const auto status = sys::channel_cancel(reply_.ends[0].selector(), std::exchange(token_, 0), true).status;
        return status == STATUS_STALE || status == STATUS_CLOSED ? STATUS_OK : status;
    }
    auto close() noexcept -> status_t {
        if (const auto status = cancel(); status != STATUS_OK) return status;
        if (const auto status = input_.close(); status != STATUS_OK) return status;
        return reply_.close();
    }
    auto begin(cap_t endpoint, CreateAuth alloc, cap_t events, msg_frame request,
               std::span<const CapXfer> caps = {}) noexcept -> status_t {
        if (active() || !endpoint) return STATUS_BUSY;
        if (caps.size() >= CHANNEL_MAX_CAPS) return STATUS_BAD_ARGS;
        if (const auto status = rt_admit(request.deadline); status != STATUS_OK) return status;
        if (sequence_ == UINT64_MAX) return STATUS_EXHAUSTED;
        if (reply_.ends[0] && events_ != events) if (const auto status = close(); status != STATUS_OK) return status;
        if (!reply_.ends[0]) {
            auto status = reply_.open(alloc, 1, CHANNEL_MAX_CAPS);
            if (status == STATUS_OK) status = input_.open(reply_.ends[0].selector(), events);
            if (status != STATUS_OK) { (void)close(); return status; }
        }
        events_ = events; endpoint_ = endpoint; request_ = request;
        request_.sequence = ++sequence_; token_ = 0;
        std::copy(caps.begin(), caps.end(), transfers_.begin()); count_ = caps.size();
        const auto status = send();
        if (status == STATUS_OK || status == STATUS_WOULD_BLOCK) return STATUS_OK;
        endpoint_ = 0; disarm(); return status;
    }
    auto begin(cap_t endpoint, const rt_env& env, msg_frame request) noexcept -> status_t {
        return begin(endpoint, env.alloc(), env.events, request);
    }
    auto poll(msg_packet& packet) noexcept -> status_t {
        if (!active()) return STATUS_INVALID_OP;
        auto status = rt_admit(request_.deadline);
        if (status == STATUS_OK && !token_) status = send();
        if (status == STATUS_OK) {
            status = input_.receive(packet);
            if (status == STATUS_OK) {
                endpoint_ = 0; token_ = 0;
                return packet.message.sequence == request_.sequence && packet.message.operation == request_.operation
                    ? static_cast<status_t>(packet.message.status) : STATUS_PEER_FAULT;
            }
        }
        if (status == STATUS_WOULD_BLOCK || status == STATUS_BUSY) {
            if (token_) {
                const auto armed = input_.arm().status;
                if (armed != STATUS_OK) { (void)close(); return armed; }
            }
            return STATUS_WOULD_BLOCK;
        }
        const auto canceled = status == STATUS_CLOSED || status == STATUS_PEER_CLOSED ? close() : cancel();
        return canceled == STATUS_OK ? status : canceled;
    }
    auto poll(msg_frame& response) noexcept -> status_t {
        msg_packet packet;
        const auto status = poll(packet);
        response = packet.message;
        return !active() && packet.count ? STATUS_PEER_FAULT : status;
    }
};

class rpc_client final {
    cap_t directory_{};
    CreateAuth pool_{};
    rpc_pending request_;
public:
    auto open(cap_t directory, CreateAuth pool) noexcept -> status_t {
        if (!directory || !pool.account || !pool.scope) return STATUS_BAD_ARGS;
        if (const auto status = request_.close(); status != STATUS_OK) return status;
        directory_ = directory; pool_ = pool;
        return STATUS_OK;
    }
    auto exchange(msg_frame& message, msg_packet& packet, std::span<const CapXfer> caps = {}) noexcept -> status_t {
        auto status = request_.begin(directory_, pool_, 0, message, caps);
        if (status != STATUS_OK) return status;
        for (;;) {
            status = request_.poll(packet);
            if (!request_.active()) { message = packet.message; return status; }
            status = request_.wait();
            if (status != STATUS_OK) { (void)request_.cancel(); return status; }
        }
    }
    auto exchange(msg_frame& message) noexcept -> status_t {
        msg_packet packet;
        const auto status = exchange(message, packet);
        return packet.count ? STATUS_PEER_FAULT : status;
    }
};
