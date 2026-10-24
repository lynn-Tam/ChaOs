#pragma once
#include <expected>
#include <ubus/ubus.hpp>
#include <deque>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/terminal_input_parser.hpp>
#include <ftxui/component/event.hpp>
#include <optional>

// Explicit, single-threaded native FTXUI session. The application owns its loop.
class ft_terminal final {
    cap_t port_{};
    ubus_conn control_;
    ch_pair subscription_;
    ubus_rx focus_events_, input_;
    uint64_t focus_{};
    bool focused_{};
    struct { uint16_t rows{24}, columns{80}; } size_;
    status_t status_{};
    std::deque<unsigned char> bytes_;
    ev_watch writable_;
    std::string frame_, next_frame_, pending_;
    size_t sent_{};
    auto reset() -> status_t {
        frame_.clear(); next_frame_.clear(); pending_.clear(); sent_ = 0; redraw_ = true;
        return writable_.close();
    }
    auto open_session(const rt_env& env, const ubus_msg& setup) -> status_t {
        port_ = rt_events();
        auto status = control_.bind(setup.capability(setup.data["message"]["terminal"]));
        if (status == STATUS_OK) status = subscription_.open(env.alloc(), 8, 0);
        if (status == STATUS_OK) status = focus_events_.open(subscription_.ends[0].selector(), port_);
        if (status == STATUS_OK) status = input_.open(setup.capability(setup.data["message"]["stdin"]), port_);
        if (status != STATUS_OK) return status;
        const auto sender = sys::channel_mint(subscription_.ends[1].selector(), env.cspace, 1, RIGHT_SEND | RIGHT_DUPLICATE);
        if (sender.status != STATUS_OK) return sender.status;
        ubus_msg request, response;
        const auto events_idx = request.offer(cap_owner{sender.value}, RIGHT_SEND);
        if (!events_idx) return events_idx.error();
        request.data.cap("events", *events_idx);
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
    // 12 requests redraw after focus is regained; negative values are errors.
    auto read_byte(uint64_t deadline = 0, bool block = true) -> int {
        for (;;) {
            ubus_msg event;
            const auto status = focus_events_.receive(event);
            if (status == STATUS_OK) {
                const auto focus = event.data["focus"].as<uint64_t>();
                const auto focused = event.data["focused"].as<bool>();
                if (!focus || !focused) return STATUS_PEER_FAULT;
                if (*focus < focus_) continue;
                focus_ = *focus; focused_ = *focused;
                if (const auto reset_status = reset(); reset_status != STATUS_OK) return reset_status;
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
                ubus_msg message;
                const auto received = input_.receive(message);
                if (received == STATUS_OK) {
                    const auto bytes = message.data["data"].as<std::span<const std::byte>>();
                    if (message.status != STATUS_OK) return message.status;
                    if (!bytes || bytes->empty()) return STATUS_CANCELED;
                    for (const auto byte : *bytes) bytes_.push_back(uint8_t(byte));
                    continue;
                }
                if (received != STATUS_WOULD_BLOCK) return received;
            }
            if (!block) return STATUS_WOULD_BLOCK;
            if (const auto wake = ev_wait(port_, deadline); wake.status != STATUS_OK) return wake.status;
        }
    }

    std::deque<ftxui::Event> events_;
    uint64_t input_at_{};
    ftxui::TerminalInputParser parser_{[this](ftxui::Event e) { input_at_ = 0; events_.push_back(std::move(e)); }};
    bool redraw_{true};
public:
    ft_terminal() = default;
    ~ft_terminal();
    auto open(const rt_env&, const ubus_msg&) -> status_t;
    auto read() -> std::expected<std::optional<ftxui::Event>, status_t>;
    auto present(const ftxui::Component&) -> status_t;
    auto flush() -> status_t;
    auto wait(uint64_t deadline = 0) -> status_t;
    auto focused() const -> bool { return focused_; }
    auto rows() const -> size_t { return size_.rows; }
    auto columns() const -> size_t { return size_.columns; }
    auto redraw() -> bool { return std::exchange(redraw_, false); }
    auto deadline() const -> uint64_t { return input_at_; }
    void print(std::string_view);
};
