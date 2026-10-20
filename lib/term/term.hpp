#pragma once
#include <lib/stream/stream.hpp>
#include <deque>
#include <libk/fmt.hpp>

inline constexpr rt_key tm_service{"terminal", ObjChannel};
struct tm_size { uint16_t rows, columns; };
inline constexpr tm_size tm_default_size{24, 80};

class tm_client {
    cap_t port_{};
    ubus_conn control_;
    ch_pair subscription_;
    ubus_rx events_, input_;
    uint64_t focus_{};
    bool focused_{};
    tm_size size_{tm_default_size};
    status_t status_{};
    std::deque<unsigned char> bytes_;
public:
    auto open(const rt_env& env) -> status_t {
        port_ = env.events;
        auto status = control_.bind(env.selector(tm_service));
        if (status == STATUS_OK) status = subscription_.open(env.alloc(), 8, 0);
        if (status == STATUS_OK) status = events_.open(subscription_.ends[0].selector(), port_);
        if (status == STATUS_OK) status = input_.open(env.selector(rt_stdin), port_);
        if (status != STATUS_OK) return status;
        const auto sender = sys::channel_mint(subscription_.ends[1].selector(), env.cspace, 1, RIGHT_SEND | RIGHT_DUPLICATE);
        if (sender.status != STATUS_OK) return sender.status;
        ubus_msg request, response;
        request.data.cap("events", *request.offer(cap_owner{sender.value}, RIGHT_SEND));
        return control_.invoke("Fullscreen", std::move(request), response);
    }
    auto write(const char* data, size_t size) -> status_t {
        while (size) {
            const auto count = std::min(size, size_t{56});
            ubus_msg frame;
            frame.data.put("focus", focus_).put("data", std::as_bytes(std::span{data, count}));
            const auto status = control_.notify("Frame", std::move(frame));
            if (status != STATUS_OK) { status_ = status; return status; }
            data += count; size -= count;
        }
        return STATUS_OK;
    }
    auto write(std::string_view text) -> status_t { return write(text.data(), text.size()); }
    void put(char byte) { (void)write(&byte, 1); }
    auto status() const -> status_t { return status_; }
    auto focused() const -> bool { return focused_; }
    auto rows() const -> size_t { return size_.rows; }
    auto columns() const -> size_t { return size_.columns; }
    // 12 requests redraw after focus is regained; negative values are errors.
    auto read(uint64_t deadline = 0, bool block = true) -> int {
        for (;;) {
            ubus_msg event;
            const auto status = events_.receive(event);
            if (status == STATUS_OK) {
                const auto focus = event.data["focus"].as<uint64_t>();
                const auto focused = event.data["focused"].as<bool>();
                if (!focus || !focused) return STATUS_PEER_FAULT;
                if (*focus < focus_) continue;
                focus_ = *focus; focused_ = *focused;
                if (focused_) {
                    const auto rows = event.data["rows"].as<uint16_t>(), columns = event.data["columns"].as<uint16_t>();
                    if (!rows || !columns || *rows < 8 || *columns < 24) return STATUS_PEER_FAULT;
                    size_ = {*rows, *columns}; return 12;
                }
                continue;
            }
            if (status != STATUS_WOULD_BLOCK) return status;
            if (focused_) {
                if (!bytes_.empty()) { const auto byte = bytes_.front(); bytes_.pop_front(); return byte; }
                str_chunk chunk;
                const auto received = str_receive(input_, chunk);
                if (received == STATUS_OK) {
                    if (!chunk.size) return STATUS_CANCELED;
                    for (size_t i = 0; i < chunk.size; ++i) bytes_.push_back(chunk.data[i]);
                    continue;
                }
                if (received != STATUS_WOULD_BLOCK) return received;
            }
            if (!block) return STATUS_WOULD_BLOCK;
            if (const auto wake = ev_wait(port_, deadline); wake.status != STATUS_OK) return wake.status;
        }
    }
};

// Bounded ANSI batch for direct terminal clients such as the editor.
class tm_painter {
    tm_client& output_;
    libk::fmt::fixed_buffer<4096> bytes_;
public:
    explicit tm_painter(tm_client& output) noexcept : output_(output) {}
    void flush() noexcept { if (bytes_.size()) { (void)output_.write(bytes_.view()); bytes_.clear(); } }
    void append(const char* data, size_t size) noexcept {
        while (size) {
            if (bytes_.size() == bytes_.capacity()) flush();
            const auto count = std::min(size, bytes_.capacity() - bytes_.size());
            (void)bytes_.write(data, count); data += count; size -= count;
        }
    }
    void text(std::string_view text) noexcept { append(text.data(), text.size()); }
    void number(size_t number) noexcept {
        libk::fmt::fixed_buffer<24> digits;
        (void)libk::fmt::format_to<"{}">(digits, number); text(digits.view());
    }
    void position(size_t row, size_t column) noexcept {
        text("\x1b["); number(row); text(";"); number(column); text("H");
    }
};
