#pragma once

#include <algorithm>
#include <array>
#include <cap.hpp>
#include <start.hpp>
#include <span>
#include <string.h>

// Hints only prompt a recheck of provider facts. Overflow requires observing
// every subscription before acknowledging the rescan epoch.
inline auto ev_wait(cap_t port, word_t deadline = 0) noexcept -> sys::SysResult {
    auto result = sys::wait_one(port, PORT_READABLE | PORT_OVERFLOW, 0, deadline);
    if (result.status != STATUS_OK) return result;
    for (;;) {
        const auto taken = sys::port_take(port);
        if (taken.status == STATUS_RETRY) break;
        if (taken.status != STATUS_OK) return taken;
    }
    const auto state = sys::source_info(port);
    if (state.status != STATUS_OK || !(state.value & PORT_OVERFLOW)) return state;
    word_t epoch{}, last{};
    for (;;) {
        const auto status = sys::port_records(port, 0, epoch, last);
        if (status.status == STATUS_STALE) return {.status = STATUS_OK};
        if (status.status != STATUS_OK) return status;
        PortRecords page;
        ::memcpy(&page, reinterpret_cast<const void*>(rt_startup().ipc), sizeof(page));
        epoch = page.epoch;
        if (!page.more) break;
        last = page.next;
    }
    const auto status = sys::port_resync(port, epoch);
    return status.status == STATUS_STALE ? sys::SysResult{.status = STATUS_OK} : status;
}

// One owned registration on a borrowed Port and Source capability.
class ev_watch final {
    cap_t port_{};
    word_t id_{};
public:
    ev_watch() = default;
    ev_watch(const ev_watch&) = delete;
    auto operator=(const ev_watch&) -> ev_watch& = delete;
    ev_watch(ev_watch&& other) noexcept { *this = std::move(other); }
    auto operator=(ev_watch&& other) noexcept -> ev_watch& {
        if (this != &other) {
            if (const auto status = close(); status != STATUS_OK) sys::exit(status);
            port_ = std::exchange(other.port_, 0); id_ = std::exchange(other.id_, 0);
        }
        return *this;
    }
    ~ev_watch() { if (const auto status = close(); status != STATUS_OK) sys::exit(status); }
    explicit operator bool() const noexcept { return id_ != 0; }
    auto close() noexcept -> status_t {
        if (!id_) return STATUS_OK;
        const auto status = sys::port_unwatch(port_, id_).status;
        if (status == STATUS_OK) { id_ = 0; port_ = 0; }
        return status;
    }
    auto open(cap_t port, cap_t source, word_t mask, word_t seen = 0, word_t flags = 0,
              word_t facet = SOURCE_STATE) noexcept -> status_t {
        if (const auto status = close(); status != STATUS_OK) return status;
        const auto result = sys::port_watch(port, source, mask, seen, 0, flags, facet);
        if (result.status == STATUS_OK) { port_ = port; id_ = result.value; }
        return result.status;
    }
    auto grant(cap_t port, cap_t scope, uint64_t object, cap_t seed, uint64_t selected, uint64_t seen) -> status_t {
        if (const auto status = close(); status != STATUS_OK) return status;
        const auto result = sys::port_grant(port, scope, object, seed, selected, seen);
        if (result.status == STATUS_OK) { port_ = port; id_ = result.value; }
        return result.status;
    }
    auto arm(word_t seen = 0) noexcept -> sys::SysResult {
        return id_ ? sys::port_arm(port_, id_, seen) : sys::SysResult{.status=STATUS_BAD_ARGS};
    }
};

// An explicit software doorbell, observed by the caller's existing Port.
class ev_notice final {
    cap_owner source_;
    ev_watch watch_;
public:
    ev_notice() noexcept = default;
    ~ev_notice() { if (const auto status = close(); status != STATUS_OK) sys::exit(status); }
    ev_notice(const ev_notice&) = delete;
    auto operator=(const ev_notice&) -> ev_notice& = delete;
    auto open(CreateAuth auth, cap_t port) noexcept -> status_t {
        if (source_) return STATUS_BUSY;
        const auto created = sys::notification_create(auth, 1);
        if (created.status != STATUS_OK) return created.status;
        source_ = cap_owner{created.value};
        return watch_.open(port, selector(), ~word_t{});
    }
    auto selector() const noexcept -> cap_t { return source_.selector(); }
    auto take() noexcept -> sys::SysResult { return sys::notification_take(selector()); }
    auto arm() noexcept -> status_t { return watch_.arm().status; }
    auto close() noexcept -> status_t {
        if (const auto status = watch_.close(); status != STATUS_OK) return status;
        if (source_) {
            const auto status = sys::object_destroy(selector()).status;
            if (status != STATUS_OK) return status;
        }
        return source_.close();
    }
};

// Own every committed transfer, independently of the payload's interpretation.
struct ch_packet final {
    std::array<word_t, CHANNEL_MAX_WORDS> words{};
    std::array<cap_owner, CHANNEL_MAX_CAPS> capabilities{};
    size_t word_count{}, count{};
    word_t badge{}, tag{};
    bool called{};
};

// A failed try accepts no request. Wait on facts, then revalidate and retry.
template<bool Send>
inline auto ch_call(cap_t channel, bool block, cap_t reply = 0) noexcept -> sys::SysResult {
    for (;;) {
        auto result = Send ? (reply ? sys::channel_call(channel, reply) : sys::channel_try_send(channel))
                           : sys::channel_try_recv(channel);
        if (!block || result.status != STATUS_WOULD_BLOCK) return result;
        result = sys::wait_one(channel, SOURCE_READY | SOURCE_TERMINAL, 0, 0, 0,
            Send ? SOURCE_WRITE : SOURCE_READ);
        if (result.status != STATUS_OK) return result;
    }
}

inline auto ch_send(cap_t channel, std::span<const word_t> words, bool block = true,
                    std::span<const CapXfer> caps = {}, word_t tag = 0, cap_t reply = 0) noexcept -> sys::SysResult {
    if (words.size() > CHANNEL_MAX_WORDS || caps.size() > CHANNEL_MAX_CAPS)
        return {.status = STATUS_BAD_ARGS};
    auto& wire = *reinterpret_cast<ChanMsg*>(rt_startup().ipc);
    ::memset(&wire, 0, offsetof(ChanMsg, words));
    wire.version = CHANNEL_VERSION;
    wire.word_count = words.size();
    wire.cap_count = caps.size();
    wire.tag = tag;
    std::copy(words.begin(), words.end(), wire.words);
    std::copy(caps.begin(), caps.end(), wire.caps);
    return ch_call<true>(channel, block, reply);
}
inline auto ch_recv(cap_t channel, ch_packet& packet, bool block = true,
                    size_t capacity = CHANNEL_MAX_CAPS) noexcept -> sys::SysResult {
    if (capacity > CHANNEL_MAX_CAPS) return {.status = STATUS_BAD_ARGS};
    packet = {};
    auto& wire = *reinterpret_cast<ChanMsg*>(rt_startup().ipc);
    ::memset(&wire, 0, offsetof(ChanMsg, words));
    wire.version = CHANNEL_VERSION;
    wire.receive_limit = capacity;
    const auto result = ch_call<false>(channel, block);
    if (result.status != STATUS_OK) return result;
    packet.called = (wire.flags & CHANNEL_FLAG_CALL) != 0;
    packet.count = wire.received_count;
    for (size_t i = 0; i < packet.count; ++i)
        packet.capabilities[i] = cap_owner{wire.received[i]};
    packet.word_count = wire.word_count;
    packet.badge = wire.sender_badge;
    packet.tag = wire.tag;
    std::copy_n(wire.words, packet.word_count, packet.words.begin());
    return result;
}

// One reader borrows a Channel and owns its readiness subscription. Draining
// to WOULD_BLOCK arms the source before returning to the enclosing event loop.
class ch_rx {
    cap_t channel_{};
    ev_watch watch_;
public:
    explicit operator bool() const { return channel_ != 0; }
    auto open(cap_t channel, cap_t events = 0) -> status_t {
        const auto status = close();
        if (status != STATUS_OK) return status;
        if (!channel) return STATUS_BAD_ARGS;
        if (events) {
            const auto observed = watch_.open(events, channel, SOURCE_READY | SOURCE_TERMINAL, 0, 0, SOURCE_READ);
            if (observed != STATUS_OK) return observed;
        }
        channel_ = channel; return STATUS_OK;
    }
    auto close() -> status_t {
        const auto status = watch_.close();
        if (status == STATUS_OK) channel_ = 0;
        return status;
    }
    auto arm() -> status_t { return watch_ ? watch_.arm().status : STATUS_OK; }
    auto receive(ch_packet& packet) -> status_t {
        const auto status = ch_recv(channel_, packet, false).status;
        if (status != STATUS_WOULD_BLOCK || !watch_) return status;
        const auto armed = watch_.arm().status;
        return armed == STATUS_OK ? status : armed;
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
    auto open(CreateAuth pool, word_t depth = 4, word_t transfers = 4,
              word_t words = CHANNEL_MAX_WORDS) noexcept
        -> status_t {
        const auto s = close();
        if (s != STATUS_OK)
            return s;
        const auto r = sys::channel_create(pool, depth, words, transfers);
        ends[0] = cap_owner{r.value};
        ends[1] = cap_owner{r.value2};
        return r.status;
    }
};
