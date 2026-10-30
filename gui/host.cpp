#include <gui/host.hpp>

void gui_host::close() {
    (void)memory_watch_.close(); (void)stream_watch_.close();
    (void)writer_.cancel(); input_ = {}; receiver_ = {}; sender_ = {}; (void)stream_.close(); (void)memory_.close(); (void)bus_.close();
    (void)pixels_.close(); free_.clear(); output_.clear(); sent_ = batch_ = input_at_ = 0; reader_ = {}; input_frame_ = {};
    phase_ = 0; retry_ = _clock_now().value + _startup().frequency / 4;
}
auto gui_host::advance() -> status_t {
    ubus_msg query, reply; auto status = STATUS_OK;
    if (!phase_) {
        if (_clock_now().value < retry_) return STATUS_WOULD_BLOCK;
        status = bus_.begin_bind(_startup().bootstrap); if (status != STATUS_OK) return status; phase_ = 1;
    }
    if (phase_ == 1) {
        status = bus_.poll_bind(); if (status != STATUS_OK) return status;
        query.data.put("name", "shared"); status = bus_.begin("Get", std::move(query)); if (status != STATUS_OK) return status; phase_ = 2;
    }
    if (phase_ == 2) {
        status = bus_.poll(reply); if (status != STATUS_OK) return status; if (reply.status != STATUS_OK) return reply.status;
        status = memory_.begin_bind(reply.capability(reply.data["connector"]), "io.shared", 1, 1); if (status != STATUS_OK) return status; phase_ = 3;
    }
    if (phase_ == 3) {
        status = memory_.poll_bind(); if (status != STATUS_OK) return status;
        status = memory_watch_.open(&_events(), memory_.endpoint(), SOURCE_TERMINAL, 0, 0, SOURCE_WRITE); if (status != STATUS_OK) return status;
        status = memory_.begin("Get", {}); if (status != STATUS_OK) return status; phase_ = 4;
    }
    if (phase_ == 4) {
        status = memory_.poll(reply); if (status != STATUS_OK) return status; if (reply.status != STATUS_OK) return reply.status;
        const auto buffer = reply.data["memory"].as<ubus_buffer>();
        if (!buffer || buffer->offset || !buffer->size || buffer->size > SIZE_MAX || buffer->size % 4096) return STATUS_PEER_FAULT;
        auto area = _vm_area::map(_vm(), reply.take(reply.data["memory"]), 0, buffer->size, VM_READ | VM_WRITE);
        if (!area) return area.error();
        pixels_ = std::move(*area);
        query.data.put("name", "display"); status = bus_.begin("Get", std::move(query)); if (status != STATUS_OK) return status; phase_ = 5;
    }
    if (phase_ == 5) {
        status = bus_.poll(reply); if (status != STATUS_OK) return status; if (reply.status != STATUS_OK) return reply.status;
        status = stream_.begin_bind(reply.capability(reply.data["connector"]), "io.stream", 1, 1); if (status != STATUS_OK) return status; phase_ = 6;
    }
    if (phase_ == 6) {
        status = stream_.poll_bind(); if (status != STATUS_OK) return status;
        status = stream_watch_.open(&_events(), stream_.endpoint(), SOURCE_TERMINAL, 0, 0, SOURCE_WRITE); if (status != STATUS_OK) return status;
        status = stream_.begin("Open", {}); if (status != STATUS_OK) return status; phase_ = 7;
    }
    if (phase_ == 7) {
        status = stream_.poll(reply); if (status != STATUS_OK) return status; if (reply.status != STATUS_OK) return reply.status;
        receiver_ = reply.take(reply.data["read"]);
        status = input_.open(receiver_.selector(), &_events()); if (status != STATUS_OK) return status;
        sender_ = reply.take(reply.data["write"]); if (!sender_) return STATUS_PEER_FAULT;
        epoch_ = _clock_now().value; phase_ = 8;
    }
    return STATUS_OK;
}
auto gui_host::send(gui_host_packet packet) -> status_t {
    if (!sender_) return STATUS_CLOSED;
    if (sent_ && !batch_ && (sent_ == output_.size() || output_.size() == output_.capacity())) {
        output_.erase(output_.begin(), output_.begin() + sent_); sent_ = 0;
    }
    if (output_.size() == output_.capacity()) return STATUS_WOULD_BLOCK;
    packet.epoch = epoch_; output_.push_back(packet); return STATUS_OK;
}
auto gui_host::receive(gui_host_packet& packet) -> status_t {
    for (size_t n = 0; n < 32; ++n) {
        if (input_at_ == input_frame_.size) {
            const auto status = ubus_recv(input_, input_frame_);
            if (status != STATUS_OK) return status;
            input_at_ = 0;
            if (input_frame_.status != STATUS_OK) return input_frame_.status;
            if (!input_frame_.size) return STATUS_PEER_CLOSED;
        }
        const auto size = std::min(reader_.tail().size(), input_frame_.size - input_at_);
        ::memcpy(reader_.tail().data(), input_frame_.data + input_at_, size); input_at_ += size;
        if (!reader_.advance(size)) continue;
        if (reader_.packet.epoch != epoch_) continue;
        if (reader_.packet.type != gh_ack && reader_.packet.type != gh_input) return STATUS_PEER_FAULT;
        packet = reader_.packet; return STATUS_OK;
    }
    return STATUS_WOULD_BLOCK;
}
auto gui_host::poll(size_t capacity) -> status_t {
    const auto before = phase_;
    auto status = phase_ < 8 ? advance() : STATUS_OK;
    if (status != STATUS_OK) return status;
    if (phase_ >= 8) {
        status = stream_.state(); if (status == STATUS_OK) status = memory_.state();
        if (status != STATUS_OK) return status;
        if (before < 8) {
            gui_host_packet hello; hello.type = gh_hello; hello.offset = pixels_.size; hello.flags = capacity;
            if ((status = send(hello)) != STATUS_OK) return status;
        }
        for (size_t n = 0; sent_ < output_.size() && n < 32; ++n) {
            if (!batch_) batch_ = std::min(output_.size() - sent_, ubus_stream_limit / sizeof(gui_host_packet));
            ubus_frame frame; frame.size = batch_ * sizeof(gui_host_packet);
            ::memcpy(frame.data, &output_[sent_], frame.size);
            status = writer_.send(sender_.selector(), frame, false);
            if (status == STATUS_WOULD_BLOCK) break;
            if (status != STATUS_OK) return status;
            sent_ += batch_; batch_ = 0;
        }
        if (phase_ == 8) {
            gui_host_packet packet;
            status = receive(packet); if (status != STATUS_OK) return status;
            if (packet.type != gh_ack || packet.flags != gh_hello || packet.width < 64 || packet.height < 96 || !gui_bytes(packet.width, packet.height)) return STATUS_PEER_FAULT;
            width = packet.width; height = packet.height; free_.clear(); free_.emplace(0, pixels_.size); phase_ = 9;
        }
    }
    return STATUS_OK;
}
auto gui_host::allocate(uint64_t bytes) -> std::optional<uint64_t> {
    if (!ready() || !bytes || bytes % 4096) return {};
    for (auto it = free_.begin(); it != free_.end(); ++it) if (it->second >= bytes) {
        const auto offset = it->first, length = it->second; free_.erase(it);
        if (length > bytes) free_.emplace(offset + bytes, length - bytes);
        return offset;
    }
    return {};
}
void gui_host::release(uint64_t offset, uint64_t bytes) {
    auto next = free_.lower_bound(offset);
    if (next != free_.end() && offset + bytes == next->first) { bytes += next->second; next = free_.erase(next); }
    if (next != free_.begin()) {
        auto previous = std::prev(next);
        if (previous->first + previous->second == offset) { previous->second += bytes; return; }
    }
    free_.emplace(offset, bytes);
}
