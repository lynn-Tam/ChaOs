#pragma once
#include <gui/host_protocol.hpp>
#include <ubus/stream.hpp>
#include <map>

// Owns the host connection and shared staging allocator. Window/session policy
// stays in gui_server; applications never receive this shared memory authority.
class gui_host {
    ubus_conn bus_, memory_, stream_;
    _ev_watch memory_watch_, stream_watch_;
    _vm_area pixels_;
    _cap_owner receiver_, sender_;
    ubus_rx input_;
    ubus_tx writer_;
    ubus_frame input_frame_;
    size_t input_at_{};
    gui_host_reader reader_;
    std::vector<gui_host_packet> output_;
    size_t sent_{}, batch_{};
    std::map<uint64_t, uint64_t> free_;
    uint64_t epoch_{}, retry_{};
    unsigned phase_{};
    auto advance() -> status_t;
public:
    uint32_t width{}, height{};
    gui_host() { output_.reserve(512); }
    auto poll(size_t capacity) -> status_t;
    auto receive(gui_host_packet&) -> status_t;
    auto send(gui_host_packet) -> status_t;
    auto writable() const -> bool { return sender_ && (output_.size() < output_.capacity() || (sent_ && !batch_)); }
    void close();
    auto ready() const -> bool { return phase_ == 9; }
    auto epoch() const -> uint64_t { return epoch_; }
    auto deadline() const -> uint64_t { return phase_ ? UINT64_MAX : retry_; }
    auto data() const -> uintptr_t { return pixels_.address; }
    auto allocate(uint64_t bytes) -> std::optional<uint64_t>;
    void release(uint64_t offset, uint64_t bytes);
};
