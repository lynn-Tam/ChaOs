#pragma once

#include <algorithm>
#include <array>
#include <cap.hpp>
#include <start.hpp>
#include <span>
#include <libk/intrusive_tree.hpp>
#include <new>
#include <memory>
#include <mutex>
#include <string.h>

// Accepted effects and cleanup survive the caller. Waiting is a userspace
// composition over the common Op source, independent of provider type.
// Completion remains queryable after its notification source has closed.
inline auto _op_wait(cap_t op, word_t deadline = 0) noexcept -> _sys_result {
    for (;;) {
        const auto result = _op_query(op);
        if (result.value2 == OP_DONE || result.status != STATUS_PENDING) return result;
        const auto waited = _wait_one(op, OP_DONE, 0, deadline, 0, SOURCE_OP);
        if (waited.status != STATUS_OK && waited.status != STATUS_CLOSED) return waited;
    }
}

// Native watch generations route hints; cached events never replace provider
// facts. Storage is admitted with the watch, so delivery allocates nothing.
struct _ev_port {
private:
    friend class _ev_watch;
    friend auto _ev_wait(_ev_port*, word_t) noexcept -> _sys_result;
    struct entry {
        PortEvent event{};
        word_t id{}, covered{}, seen{};
        bool dirty{}, first{true}, observed{};
        libk::intrusive_tree_hook hook;
    };
    struct compare {
        auto operator()(const entry& a, const entry& b) const noexcept -> bool { return a.id < b.id; }
        auto operator()(word_t a, const entry& b) const noexcept -> bool { return a < b.id; }
        auto operator()(const entry& a, word_t b) const noexcept -> bool { return a.id < b; }
    };
    _cap_owner cap_;
    _mutex mutex;
    libk::intrusive_tree<entry, &entry::hook, compare> watches;
    word_t epoch{}, last{}, limit{};
    size_t ready{}, covered{};
    bool scanned{};
    // These two counts are derived solely here, under mutex. Dispatch and
    // overflow completion never scan unrelated registrations.
    void mark(entry& node, bool dirty, word_t watermark) noexcept {
        ready -= node.dirty; covered -= bool(node.covered);
        node.dirty = dirty; node.covered = watermark;
        ready += node.dirty; covered += bool(node.covered);
    }
public:
    _ev_port() = default;
    // Takes sole dispatcher responsibility, including every native watch.
    explicit _ev_port(_cap_owner slot) : cap_(std::move(slot)) {}
    _ev_port(const _ev_port&) = delete;
    auto operator=(const _ev_port&) -> _ev_port& = delete;
    ~_ev_port() { if (!watches.empty()) _thread_exit(STATUS_BUSY); }
    auto selector() const noexcept -> cap_t { return cap_.selector(); }
    auto open(CreateAuth auth, word_t capacity = 64, word_t hints = 16) noexcept -> status_t {
        if (cap_) return STATUS_BUSY;
        const auto result = _port_create(auth, capacity, hints);
        cap_ = _cap_owner{result.value};
        return result.status == STATUS_PENDING ? _op_wait(cap_.selector()).status : result.status;
    }
};
inline auto _ev_wait(_ev_port* state, word_t deadline = 0) noexcept -> _sys_result {
    if (!state) return {.status=STATUS_BAD_ARGS};
    const auto port = state->selector();
    for (;;) {
        {
            std::lock_guard lock{state->mutex};
            for (size_t n = 0; n < 32; ++n) {
                const auto status = _port_take(port).status;
                if (status == STATUS_RETRY) break;
                if (status != STATUS_OK) return {.status=status};
                PortEvent event; ::memcpy(&event, reinterpret_cast<const void*>(_ipc()), sizeof(event));
                if (auto* found = state->watches.find(event.watch_gen)) {
                    auto& entry = *found;
                    if (!entry.observed || event.sequence > entry.seen || (event.flags & PORT_EVENT_CLOSED)) {
                        entry.event = event; state->mark(entry, true, entry.covered);
                    }
                } else return {.status=STATUS_BAD_ARGS}; // A raw consumer must own a separate Port.
            }
            const auto facts = _source_info(port);
            if (facts.status != STATUS_OK) return facts;
            if ((facts.value & PORT_OVERFLOW) && !state->scanned) {
                for (size_t n = 0; n < 8; ++n) {
                    const auto status = _port_records(port, 0, state->epoch, state->last, state->limit).status;
                    if (status != STATUS_OK) return {.status=status};
                    PortRecords page; ::memcpy(&page, reinterpret_cast<const void*>(_ipc()), sizeof(page));
                    state->epoch = page.epoch; state->limit = page.limit; state->last = page.next;
                    for (const auto& event : std::span{page.events, page.count}) {
                        if (auto* found = state->watches.find(event.watch_gen)) {
                            auto& entry = *found;
                            if (entry.dirty || (event.flags & PORT_EVENT_CLOSED) ||
                                ((event.flags & PORT_EVENT_PENDING) && (!entry.observed || event.sequence > entry.seen))) {
                                entry.event = event; state->mark(entry, true, page.epoch);
                            }
                        } else return {.status=STATUS_BAD_ARGS};
                    }
                    if (!page.more) { state->scanned = true; break; }
                }
            }
            if (state->scanned && !state->covered) {
                const auto status = _port_resync(port, state->epoch).status;
                if (status != STATUS_OK) return {.status=status};
                state->epoch = state->last = state->limit = 0; state->scanned = false;
            }
            if (state->ready
                || (facts.value & (PORT_READABLE | PORT_OVERFLOW))) return {.status=STATUS_OK};
        }
        const auto waited = _wait_one(port, PORT_READABLE | PORT_OVERFLOW, 0, deadline);
        if (waited.status != STATUS_OK) return waited;
    }
}

// One owned registration on a borrowed Port and Source capability.
class _ev_watch final {
    _ev_port* state_{};
    std::unique_ptr<_ev_port::entry> node_;
public:
    _ev_watch() = default;
    _ev_watch(const _ev_watch&) = delete;
    auto operator=(const _ev_watch&) -> _ev_watch& = delete;
    _ev_watch(_ev_watch&& other) noexcept { *this = std::move(other); }
    auto operator=(_ev_watch&& other) noexcept -> _ev_watch& {
        if (this != &other) {
            if (const auto status = close(); status != STATUS_OK) _thread_exit(status);
            state_ = std::exchange(other.state_, nullptr); node_ = std::move(other.node_);
        }
        return *this;
    }
    ~_ev_watch() { if (const auto status = close(); status != STATUS_OK) _thread_exit(status); }
    explicit operator bool() const noexcept { return bool(node_); }
    auto pending() const noexcept -> bool {
        if (!state_) return false;
        std::lock_guard lock{state_->mutex};
        const auto& entry = *node_; return entry.dirty || entry.first;
    }
    auto event() const noexcept -> std::optional<PortEvent> {
        if (!state_) return {};
        std::lock_guard lock{state_->mutex};
        const auto& entry = *node_;
        return entry.dirty ? std::optional{entry.event} : std::nullopt;
    }
    void ack() const noexcept {
        if (!state_) return;
        std::lock_guard lock{state_->mutex};
        state_->mark(*node_, false, 0); node_->first = false;
    }
    auto close() noexcept -> status_t {
        if (!node_) return STATUS_OK;
        {
            std::lock_guard lock{state_->mutex};
            const auto status = _port_unwatch(state_->selector(), node_->id).status;
            if (status != STATUS_OK) return status;
            state_->mark(*node_, false, 0); state_->watches.erase(*node_); node_.reset();
        }
        state_ = nullptr; return STATUS_OK;
    }
    auto open(_ev_port* port, cap_t source, word_t mask, word_t seen = 0, word_t flags = 0,
              word_t facet = SOURCE_STATE, word_t key = 0) noexcept -> status_t {
        return install(port, [&] { return _port_watch(port->selector(), source, mask, seen, key, flags, facet); });
    }
    auto grant(_ev_port* port, cap_t scope, uint64_t object, cap_t seed, uint64_t selected, uint64_t seen) -> status_t {
        return install(port, [&] { return _port_grant(port->selector(), scope, object, seed, selected, seen); });
    }
    auto arm(word_t seen = 0) const noexcept -> _sys_result {
        if (!node_) return {.status=STATUS_BAD_ARGS};
        std::lock_guard lock{state_->mutex};
        const auto result = _port_arm(state_->selector(), node_->id, seen);
        if (result.status == STATUS_OK || result.status == STATUS_CLOSED) {
            auto& entry = *node_;
            state_->mark(entry, result.status == STATUS_OK && result.value, 0);
            entry.first = false;
            if (result.status == STATUS_OK) { entry.seen = result.value2; entry.observed = true; }
        }
        return result;
    }
private:
    template<class Create> auto install(_ev_port* state, Create create) -> status_t {
        if (!state) return STATUS_BAD_ARGS;
        if (const auto status = close(); status != STATUS_OK) return status;
        std::unique_ptr<_ev_port::entry> node{new (std::nothrow) _ev_port::entry};
        if (!node) return STATUS_NO_MEMORY;
        std::lock_guard lock{state->mutex};
        const auto result = create();
        if (result.status == STATUS_OK) {
            node->id = result.value; state->watches.insert(*node);
            state_ = state; node_ = std::move(node);
        }
        return result.status;
    }

};

// An explicit software doorbell, observed by the caller's existing Port.
class _ev_notice final {
    _cap_owner source_;
    _ev_watch watch_;
public:
    _ev_notice() noexcept = default;
    ~_ev_notice() { if (const auto status = close(); status != STATUS_OK) _thread_exit(status); }
    _ev_notice(const _ev_notice&) = delete;
    auto operator=(const _ev_notice&) -> _ev_notice& = delete;
    auto open(CreateAuth auth, _ev_port* port) noexcept -> status_t {
        if (source_) return STATUS_BUSY;
        const auto created = _notification_create(auth, 1);
        if (created.status != STATUS_OK) return created.status;
        source_ = _cap_owner{created.value};
        return watch_.open(port, selector(), ~word_t{});
    }
    auto selector() const noexcept -> cap_t { return source_.selector(); }
    auto take() noexcept -> _sys_result { return _notification_take(selector()); }
    auto arm() noexcept -> status_t { return watch_.arm().status; }
    auto close() noexcept -> status_t {
        if (const auto status = watch_.close(); status != STATUS_OK) return status;
        if (source_) {
            const auto status = _object_destroy(selector()).status;
            if (status != STATUS_OK) return status;
        }
        return source_.close();
    }
};

// Own every committed transfer, independently of the payload's interpretation.
struct _ch_packet final {
    std::array<word_t, CHANNEL_MAX_WORDS> words{};
    std::array<_cap_owner, CHANNEL_MAX_CAPS> capabilities{};
    size_t word_count{}, count{};
    word_t badge{}, tag{};
    bool called{};
};

// A failed try accepts no request. Wait on facts, then revalidate and retry.
template<bool Send>
inline auto _ch_call(cap_t channel, bool block, cap_t reply, word_t deadline) noexcept -> _sys_result {
    for (;;) {
        auto result = Send ? (reply ? _channel_call(channel, reply) : _channel_try_send(channel))
                           : _channel_try_recv(channel);
        if (!block || result.status != STATUS_WOULD_BLOCK) return result;
        result = _wait_one(channel, SOURCE_READY | SOURCE_TERMINAL, 0, deadline, 0,
            Send ? SOURCE_WRITE : SOURCE_READ);
        if (result.status != STATUS_OK) return result;
    }
}

inline auto _ch_send_impl(cap_t channel, std::span<const word_t> words, bool block,
                    std::span<const CapXfer> caps, word_t tag, cap_t reply, word_t deadline) noexcept -> _sys_result {
    if (words.size() > CHANNEL_MAX_WORDS || caps.size() > CHANNEL_MAX_CAPS)
        return {.status = STATUS_BAD_ARGS};
    auto& wire = *reinterpret_cast<ChanMsg*>(_ipc());
    ::memset(&wire, 0, offsetof(ChanMsg, words));
    wire.version = CHANNEL_VERSION;
    wire.word_count = words.size();
    wire.cap_count = caps.size();
    wire.tag = tag;
    std::copy(words.begin(), words.end(), wire.words);
    std::copy(caps.begin(), caps.end(), wire.caps);
    return _ch_call<true>(channel, block, reply, deadline);
}
inline auto _ch_recv_impl(cap_t channel, _ch_packet& packet, bool block,
                    size_t capacity, word_t deadline) noexcept -> _sys_result {
    if (capacity > CHANNEL_MAX_CAPS) return {.status = STATUS_BAD_ARGS};
    packet = {};
    auto& wire = *reinterpret_cast<ChanMsg*>(_ipc());
    ::memset(&wire, 0, offsetof(ChanMsg, words));
    wire.version = CHANNEL_VERSION;
    wire.receive_limit = capacity;
    const auto result = _ch_call<false>(channel, block, 0, deadline);
    if (result.status != STATUS_OK) return result;
    packet.called = (wire.flags & CHANNEL_FLAG_CALL) != 0;
    packet.count = wire.received_count;
    for (size_t i = 0; i < packet.count; ++i)
        packet.capabilities[i] = _cap_owner{wire.received[i]};
    packet.word_count = wire.word_count;
    packet.badge = wire.sender_badge;
    packet.tag = wire.tag;
    std::copy_n(wire.words, packet.word_count, packet.words.begin());
    return result;
}

inline auto _ch_try_send(cap_t channel, std::span<const word_t> words,
    std::span<const CapXfer> caps = {}, word_t tag = 0, cap_t reply = 0) noexcept -> _sys_result {
    return _ch_send_impl(channel, words, false, caps, tag, reply, 0);
}
inline auto _ch_send(cap_t channel, std::span<const word_t> words,
    std::span<const CapXfer> caps = {}, word_t tag = 0, cap_t reply = 0, word_t deadline = 0) noexcept -> _sys_result {
    return _ch_send_impl(channel, words, true, caps, tag, reply, deadline);
}
inline auto _ch_try_recv(cap_t channel, _ch_packet& packet, size_t capacity = CHANNEL_MAX_CAPS) noexcept -> _sys_result {
    return _ch_recv_impl(channel, packet, false, capacity, 0);
}
inline auto _ch_recv(cap_t channel, _ch_packet& packet, size_t capacity = CHANNEL_MAX_CAPS, word_t deadline = 0) noexcept -> _sys_result {
    return _ch_recv_impl(channel, packet, true, capacity, deadline);
}

// One reader borrows a Channel and owns its readiness subscription. Draining
// to WOULD_BLOCK arms the source before returning to the enclosing event loop.
class _ch_rx {
    cap_t channel_{};
    _ev_watch watch_;
public:
    explicit operator bool() const { return channel_ != 0; }
    auto open(cap_t channel, _ev_port* events = nullptr) -> status_t {
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
    auto drop(word_t sequence) -> status_t { return _channel_drop(channel_, sequence).status; }
    auto receive(_ch_packet& packet) -> _sys_result {
        auto result = _ch_try_recv(channel_, packet);
        if (result.status == STATUS_WOULD_BLOCK && watch_) {
            const auto armed = watch_.arm().status;
            if (armed != STATUS_OK) result.status = armed;
        }
        return result;
    }
};

// Own the channel object, not merely its local CSpace slots.
class _ch_pair final {
  public:
    std::array<_cap_owner, 2> ends;
    _ch_pair() noexcept = default;
    _ch_pair(_ch_pair&&) noexcept = default;
    auto operator=(_ch_pair&&) noexcept -> _ch_pair& = default;
    _ch_pair(const _ch_pair&) = delete;
    auto operator=(const _ch_pair&) -> _ch_pair& = delete;
    ~_ch_pair() {
        if (const auto rc = close(); rc != STATUS_OK)
            _thread_exit(rc);
    }
    auto close() noexcept -> status_t {
        const auto root = ends[0] ? ends[0].selector() : ends[1].selector();
        if (root) {
            const auto s = _object_destroy(root).status;
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
        const auto r = _channel_create(pool, depth, words, transfers);
        ends[0] = _cap_owner{r.value};
        ends[1] = _cap_owner{r.value2};
        return r.status == STATUS_PENDING ? _op_wait(ends[0].selector()).status : r.status;
    }
};
