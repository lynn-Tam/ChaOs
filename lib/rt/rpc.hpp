#pragma once
#include <ipc.hpp>
#include <util.hpp>

// One in-flight call owns one persistent reply transport. Request capabilities
// remain borrowed until send commits; the enclosing message keeps them alive.
class rpc_op {
    ch_pair reply_;
    ev_watch readable_, writable_;
    cap_t endpoint_{}, events_{};
    std::array<word_t, CHANNEL_MAX_WORDS> words_{};
    std::array<CapXfer, CHANNEL_MAX_CAPS - 1> caps_{};
    size_t size_{}, count_{};
    word_t tag_{}, deadline_{}, token_{};
    bool drain_{}, committed_{};

    void disarm() noexcept {
        if (const auto status = writable_.close(); status != STATUS_OK) sys::exit(status);
    }
    auto send() noexcept -> status_t {
        const auto result = ch_send(endpoint_, std::span{words_}.first(size_), false,
            std::span{caps_}.first(count_), tag_, reply_.ends[1].selector());
        if (result.status == STATUS_OK) { token_ = result.value; committed_ = true; disarm(); return STATUS_OK; }
        if (result.status != STATUS_WOULD_BLOCK && result.status != STATUS_BUSY) return result.status;
        if (!events_) return STATUS_WOULD_BLOCK;
        const auto status = writable_ ? writable_.arm().status : writable_.open(events_, endpoint_,
            SOURCE_READY | SOURCE_TERMINAL, 0, 0, SOURCE_WRITE);
        return status == STATUS_OK ? STATUS_WOULD_BLOCK : status;
    }
public:
    rpc_op() = default;
    rpc_op(const rpc_op&) = delete;
    auto operator=(const rpc_op&) -> rpc_op& = delete;
    ~rpc_op() { if (const auto status = close(); status != STATUS_OK) sys::exit(status); }
    auto active() const noexcept -> bool { return endpoint_ != 0; }
    auto sent() const noexcept -> bool { return committed_; }
    auto deadline() const noexcept -> word_t { return active() && deadline_ && (!token_ || !drain_) ? deadline_ : UINT64_MAX; }
    auto cancel() noexcept -> status_t {
        disarm(); endpoint_ = 0;
        if (!token_) return STATUS_OK;
        const auto status = sys::channel_cancel(reply_.ends[0].selector(), std::exchange(token_, 0), true).status;
        return status == STATUS_STALE || status == STATUS_CLOSED ? STATUS_OK : status;
    }
    auto close() noexcept -> status_t {
        if (const auto status = cancel(); status != STATUS_OK) return status;
        if (const auto status = readable_.close(); status != STATUS_OK) return status;
        return reply_.close();
    }
    auto begin(cap_t endpoint, CreateAuth auth, cap_t events, std::span<const word_t> words,
               std::span<const CapXfer> caps = {}, word_t tag = 0, word_t deadline = 0, bool drain = false) noexcept -> status_t {
        if (active()) return STATUS_BUSY;
        if (!endpoint || words.size() > words_.size() || caps.size() > caps_.size()) return STATUS_BAD_ARGS;
        if (const auto status = rt_admit(deadline); status != STATUS_OK) return status;
        if (reply_.ends[0] && events != events_)
            if (const auto status = close(); status != STATUS_OK) return status;
        if (!reply_.ends[0]) {
            auto status = reply_.open(auth, 1, CHANNEL_MAX_CAPS);
            if (status == STATUS_OK && events) status = readable_.open(events, reply_.ends[0].selector(),
                SOURCE_READY | SOURCE_TERMINAL, 0, 0, SOURCE_READ);
            if (status != STATUS_OK) { (void)close(); return status; }
        }
        events_ = events; endpoint_ = endpoint; tag_ = tag; deadline_ = deadline; drain_ = drain; committed_ = false;
        std::copy(words.begin(), words.end(), words_.begin()); size_ = words.size();
        std::copy(caps.begin(), caps.end(), caps_.begin()); count_ = caps.size();
        const auto status = send();
        if (status == STATUS_OK || status == STATUS_WOULD_BLOCK) return STATUS_OK;
        endpoint_ = 0; disarm(); return status;
    }
    auto poll(ch_packet& packet) noexcept -> status_t {
        if (!active()) return STATUS_INVALID_OP;
        auto status = token_ && drain_ ? STATUS_OK : rt_admit(deadline_);
        if (status == STATUS_OK && !token_) status = send();
        if (status == STATUS_OK) {
            status = ch_recv(reply_.ends[0].selector(), packet, false).status;
            if (status == STATUS_OK) { endpoint_ = token_ = 0; return STATUS_OK; }
        }
        if (status == STATUS_WOULD_BLOCK || status == STATUS_BUSY) {
            if (token_ && readable_) {
                const auto armed = readable_.arm().status;
                if (armed != STATUS_OK) { (void)close(); return armed; }
            }
            return STATUS_WOULD_BLOCK;
        }
        const auto canceled = status == STATUS_CLOSED || status == STATUS_PEER_CLOSED ? close() : cancel();
        return canceled == STATUS_OK ? status : canceled;
    }
    auto wait() noexcept -> status_t {
        if (!active()) return STATUS_INVALID_OP;
        return sys::wait_one(token_ ? reply_.ends[0].selector() : endpoint_,
            SOURCE_READY | SOURCE_TERMINAL, 0, token_ && drain_ ? 0 : deadline_, 0, token_ ? SOURCE_READ : SOURCE_WRITE).status;
    }
    auto commit() noexcept -> status_t {
        while (active() && !sent()) {
            auto status = rt_admit(deadline_);
            if (status == STATUS_OK) status = send();
            if (status != STATUS_WOULD_BLOCK) return status;
            status = wait();
            if (status != STATUS_OK) return status;
        }
        return active() ? STATUS_OK : STATUS_INVALID_OP;
    }
};
