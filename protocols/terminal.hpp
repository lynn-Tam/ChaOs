#pragma once
#include <protocols/stream.hpp>
#include <deque>
#include <string_view>
#include <libk/fmt.hpp>

inline constexpr rt_req tm_service{"terminal", 0x5445524d, ObjChannel};
enum class tm_op : uint64_t { Fullscreen = 16, FocusLost, FocusGained, Frame };
struct tm_size { uint16_t rows, columns; };
inline constexpr tm_size tm_default_size{24, 80}; // Supported serial ANSI viewport.

// Explicit session client. Focus events never enter the ordinary stdin stream.
class tm_client {
    cap_t channel_{}, events_{};
    msg_rx control_, input_;
    uint64_t focus_{};
    bool focused_{};
    tm_size size_{tm_default_size};
    status_t status_{};
    std::deque<unsigned char> bytes_;
public:
    auto open(const rt_env& env) noexcept -> status_t {
        channel_ = env.selector(tm_service); events_ = env.events;
        if (!channel_) return STATUS_INVALID_OP;
        auto status = control_.open(channel_, events_);
        if (status == STATUS_OK) status = input_.open(env.selector(rt_stdin), events_);
        if (status == STATUS_OK) status = msg_send(channel_, {.operation = static_cast<uint64_t>(tm_op::Fullscreen)}).status;
        return status;
    }
    auto write(const char* data, size_t size) noexcept -> status_t {
        while (size) {
            msg_frame frame{.operation = static_cast<uint64_t>(tm_op::Frame), .value = focus_};
            frame.size = std::min(size, sizeof(frame.data));
            ::memcpy(frame.data, data, frame.size);
            const auto status = msg_send(channel_, frame).status;
            if (status != STATUS_OK) { status_ = status; return status; }
            data += frame.size; size -= frame.size;
        }
        return STATUS_OK;
    }
    auto write(std::string_view text) noexcept -> status_t { return write(text.data(), text.size()); }
    void put(char byte) noexcept { (void)write(&byte, 1); }
    auto status() const noexcept -> status_t { return status_; }
    // 12 requests a complete redraw after gaining focus. Negative values are
    // syscall errors, including a sampling deadline expiry for native TUIs.
    auto focused() const noexcept -> bool { return focused_; }
    auto rows() const noexcept -> size_t { return size_.rows; }
    auto columns() const noexcept -> size_t { return size_.columns; }
    auto read(uint64_t deadline = 0, bool block = true) noexcept -> int {
        for (;;) {
            msg_frame message;
            const auto event = control_.try_receive(message);
            if (event.status == STATUS_OK) {
                if (message.value < focus_) continue;
                focus_ = message.value;
                if (message.operation == static_cast<uint64_t>(tm_op::FocusLost)) focused_ = false;
                if (message.operation == static_cast<uint64_t>(tm_op::FocusGained)) {
                    if (message.size != sizeof(size_)) return STATUS_BAD_ARGS;
                    ::memcpy(&size_, message.data, sizeof(size_));
                    if (size_.rows < 8 || size_.columns < 24) return STATUS_BAD_ARGS;
                    focused_ = true; return 12;
                }
                continue;
            }
            if (event.status != STATUS_WOULD_BLOCK) return event.status;
            if (focused_) {
                if (!bytes_.empty()) { const auto byte = bytes_.front(); bytes_.pop_front(); return byte; }
                const auto input = input_.try_receive(message);
                if (input.status == STATUS_OK) {
                    if (message.operation == static_cast<uint64_t>(str_frame::End)) return STATUS_CANCELED;
                    for (size_t i = 0; i < message.size; ++i) bytes_.push_back(message.data[i]);
                    continue;
                }
                if (input.status != STATUS_WOULD_BLOCK) return input.status;
                if (const auto status = input_.arm().status; status != STATUS_OK) return status;
            }
            if (const auto status = control_.arm().status; status != STATUS_OK) return status;
            if (!block) return STATUS_WOULD_BLOCK;
            if (const auto status = ev_wait(events_, deadline).status; status != STATUS_OK) return status;
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
