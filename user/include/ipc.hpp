#pragma once

#include <algorithm>
#include <array>
#include <cap.hpp>
#include <start.hpp>
#include <span>

// Own every committed transfer, independently of the payload's interpretation.
struct ch_packet final {
    std::array<word_t, CHANNEL_MAX_WORDS> words{};
    std::array<cap_owner, CHANNEL_MAX_CAPS> capabilities{};
    size_t word_count{}, count{};
    word_t badge{}, tag{};
};

inline auto ch_send(cap_t channel, std::span<const word_t> words, bool block = true,
                    std::span<const CapXfer> caps = {}, word_t tag = 0) noexcept -> sys::SysResult {
    if (words.size() > CHANNEL_MAX_WORDS || caps.size() > CHANNEL_MAX_CAPS)
        return {.status = STATUS_BAD_ARGS};
    auto& wire = *reinterpret_cast<ChanMsg*>(rt_startup().ipc);
    wire = {};
    wire.version = CHANNEL_VERSION;
    wire.word_count = words.size();
    wire.cap_count = caps.size();
    wire.tag = tag;
    std::copy(words.begin(), words.end(), wire.words);
    std::copy(caps.begin(), caps.end(), wire.caps);
    return block ? sys::channel_send(channel) : sys::channel_try_send(channel);
}
inline auto ch_recv(cap_t channel, ch_packet& packet, bool block = true,
                    size_t capacity = CHANNEL_MAX_CAPS) noexcept -> sys::SysResult {
    if (capacity > CHANNEL_MAX_CAPS) return {.status = STATUS_BAD_ARGS};
    packet = {};
    auto& wire = *reinterpret_cast<ChanMsg*>(rt_startup().ipc);
    wire = {};
    wire.version = CHANNEL_VERSION;
    wire.receive_limit = capacity;
    const auto result = block ? sys::channel_recv(channel) : sys::channel_try_recv(channel);
    if (result.status != STATUS_OK) return result;
    packet.count = wire.received_count;
    for (size_t i = 0; i < packet.count; ++i)
        packet.capabilities[i] = cap_owner{wire.received[i]};
    packet.word_count = wire.word_count;
    packet.badge = wire.sender_badge;
    packet.tag = wire.tag;
    std::copy_n(wire.words, packet.word_count, packet.words.begin());
    return result;
}

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
    auto open(cap_t pool, word_t depth = 4, word_t transfers = 4, word_t relations = 2,
              word_t words = CHANNEL_MAX_WORDS) noexcept
        -> status_t {
        const auto s = close();
        if (s != STATUS_OK)
            return s;
        const auto r = sys::channel_create(pool, depth, words, transfers, relations);
        ends[0] = cap_owner{r.value};
        ends[1] = cap_owner{r.value2};
        return r.status;
    }
};
