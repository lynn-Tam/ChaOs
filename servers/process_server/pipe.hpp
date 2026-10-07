#include <optional>
#include <servers/runtime/service.hpp>
#pragma once

#include <sys/handle.hpp>
#include <sys/channel.hpp>

namespace sys::process {
// The supervisor owns the channel independently of either task's pool. Root
// grants remain alive until the consumer has drained End or exited.
class Pipe final : private libk::noncopyable_nonmovable {
    cap::OwnedCap roots_[2];
    cap::OwnedCap writer_, reader_;
    word_t writable_{};
    uint64_t sequence_{};
    std::optional<status_t> end_;
public:
    uint64_t producer{}, consumer{}; // protocol participants, not task state
    auto active() const noexcept -> bool { return static_cast<bool>(roots_[0]); }
    auto writer() const noexcept -> cap_t { return writer_.selector(); }
    auto reader() const noexcept -> cap_t { return reader_.selector(); }
    auto open(cap_t pool, cap_t cspace, cap_t events, size_t depth = 4) noexcept -> status_t {
        const auto pair = channel_create(pool, depth, CHANNEL_MAX_WORDS, 0, 2);
        if (pair.status != STATUS_OK) return pair.status;
        roots_[0] = cap::OwnedCap{{pair.value, 0}};
        roots_[1] = cap::OwnedCap{{pair.value2, 0}};
        const auto tx = channel_mint(pair.value, cspace, 1, RIGHT_SEND | RIGHT_DUPLICATE);
        if (tx.status != STATUS_OK) { close(); return tx.status; }
        writer_ = cap::OwnedCap{{tx.value, 0}};
        const auto rx = channel_mint(pair.value2, cspace, 1, RIGHT_RECEIVE | RIGHT_DUPLICATE);
        if (rx.status != STATUS_OK) { close(); return rx.status; }
        reader_ = cap::OwnedCap{{rx.value, 0}};
        const auto bound = channel_bind(writer(), events, CHANNEL_WRITABLE);
        if (bound.status != STATUS_OK) { close(); return bound.status; }
        writable_ = bound.value;
        return STATUS_OK;
    }
    void end(status_t status) noexcept { end_ = status; producer = 0; }
    void abort() noexcept { service::require(channel_close(roots_[0].selector()).status); }
    auto poll() noexcept -> status_t {
        if (!end_) return STATUS_OK;
        service::Message eof{.operation = static_cast<uint64_t>(stream::Frame::End), .status = *end_};
        const auto sent = service::send(writer(), eof, false);
        if (sent.status == STATUS_WOULD_BLOCK) {
            const auto armed = channel_arm(writer(), writable_, sequence_);
            if (armed.status == STATUS_OK) sequence_ = armed.value;
            return armed.status;
        }
        if (sent.status == STATUS_OK || sent.status == STATUS_CLOSED
            || sent.status == STATUS_PEER_CLOSED) { end_.reset(); return STATUS_OK; }
        return sent.status;
    }
    void close() noexcept {
        if (active()) service::require(object_destroy(roots_[0].selector()).status);
        writer_ = {}; reader_ = {}; roots_[0] = {}; roots_[1] = {};
        producer = consumer = sequence_ = writable_ = 0;
        end_.reset();
    }
};
} // namespace sys::process
