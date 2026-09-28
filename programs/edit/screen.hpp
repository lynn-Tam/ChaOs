#pragma once

#include <user/lib/stream.hpp>
#include <user/lib/terminal.hpp>

namespace myos::edit {

class Buffer final {
public:
    static constexpr size_t Capacity = 128 * 1024;
    [[nodiscard]] auto size() const noexcept -> size_t { return left_ + Capacity - right_; }
    [[nodiscard]] auto cursor() const noexcept -> size_t { return left_; }
    [[nodiscard]] auto modified() const noexcept -> bool { return modified_; }
    void clean() noexcept { modified_ = false; }
    [[nodiscard]] auto before() const noexcept -> const char* { return data_; }
    [[nodiscard]] auto after() const noexcept -> const char* { return data_ + right_; }
    [[nodiscard]] auto after_size() const noexcept -> size_t { return Capacity - right_; }
    [[nodiscard]] auto at(size_t position) const noexcept -> char {
        return data_[position < left_ ? position : right_ + position - left_];
    }
    [[nodiscard]] auto append(const uint8_t* data, size_t count) noexcept -> bool {
        if (count > right_ - left_) return false;
        service::copy(data_ + left_, data, count);
        left_ += count;
        return true;
    }
    void move(size_t position) noexcept {
        while (left_ > position) data_[--right_] = data_[--left_];
        while (left_ < position) data_[left_++] = data_[right_++];
    }
    [[nodiscard]] auto insert(char byte) noexcept -> bool {
        if (left_ == right_) return false;
        data_[left_++] = byte;
        modified_ = true;
        return true;
    }
    void backspace() noexcept { if (left_ != 0) { --left_; modified_ = true; } }
    void erase() noexcept { if (right_ != Capacity) { ++right_; modified_ = true; } }
private:
    char data_[Capacity]{};
    size_t left_{}, right_{Capacity};
    bool modified_{};
};

class Screen final {
    static constexpr size_t Rows = 21, Columns = 80;
    enum Key : int { Up = 256, Down, Left, Right, Home, End, PageUp, PageDown, Delete };
    class Paint final {
        stream::Writer& output_;
        char bytes_[4096]{};
        size_t used_{};
    public:
        explicit Paint(stream::Writer& output) noexcept : output_(output) {}
        void flush() noexcept {
            if (used_ != 0) { output_.write(bytes_, used_); used_ = 0; }
        }
        void append(const char* data, size_t size) noexcept {
            while (size != 0) {
                if (used_ == sizeof(bytes_)) flush();
                const size_t count = size < sizeof(bytes_) - used_ ? size : sizeof(bytes_) - used_;
                service::copy(bytes_ + used_, data, count);
                used_ += count; data += count; size -= count;
            }
        }
        void text(const char* value) noexcept { append(value, service::length(value)); }
        void number(size_t value) noexcept {
            char digits[20]{};
            size_t size{};
            do { digits[size++] = '0' + value % 10; value /= 10; } while (value != 0);
            while (size != 0) append(&digits[--size], 1);
        }
        void position(size_t row, size_t column) noexcept {
            text("\x1b["); number(row); text(";"); number(column); text("H");
        }
    };

    Buffer& buffer_;
    terminal::ByteReader input_;
    stream::Writer& output_;
    const char* path_;
    size_t top_{};
    size_t selected_{};
    size_t column_{};
    size_t horizontal_{};
    const char* notice_{};

    [[nodiscard]] auto line_start(size_t line) const noexcept -> size_t {
        size_t current{};
        for (size_t i = 0; i < buffer_.size(); ++i)
            if (buffer_.at(i) == '\n' && ++current == line) return i + 1;
        return line == 0 ? 0 : buffer_.size() + 1;
    }
    [[nodiscard]] auto line_end(size_t start) const noexcept -> size_t {
        size_t end = start;
        while (end < buffer_.size() && buffer_.at(end) != '\n') ++end;
        return end;
    }
    void locate() noexcept {
        selected_ = 0;
        size_t start{};
        for (size_t i = 0; i < buffer_.cursor(); ++i)
            if (buffer_.at(i) == '\n') { ++selected_; start = i + 1; }
        column_ = buffer_.cursor() - start;
        if (selected_ < top_) top_ = selected_;
        if (selected_ >= top_ + Rows) top_ = selected_ - Rows + 1;
        horizontal_ = column_ < Columns ? 0 : column_ - Columns + 1;
    }
    void cursor(Paint& paint) const noexcept {
        paint.position(selected_ - top_ + 2, column_ - horizontal_ + 1);
    }
    void row(Paint& paint, size_t row_number, size_t start) const noexcept {
        paint.position(row_number + 2, 1);
        paint.text("\x1b[2K");
        if (start > buffer_.size()) { paint.text("~"); return; }
        const size_t end = line_end(start);
        for (size_t i = start + horizontal_; i < end && i < start + horizontal_ + Columns; ++i) {
            const char byte = buffer_.at(i);
            const char shown = byte >= 32 && byte < 127 ? byte : ' ';
            paint.append(&shown, 1);
        }
    }
    void footer(Paint& paint) const noexcept {
        paint.position(23, 1); paint.text("\x1b[2K");
        paint.text(buffer_.modified() ? "Modified  " : "Saved     ");
        const size_t length = service::length(path_);
        paint.append(path_, length < 48 ? length : 48);
        if (notice_) { paint.text("  "); paint.text(notice_); }
        paint.position(24, 1); paint.text("\x1b[2KCtrl+O Save   Ctrl+X Exit   Arrows Move   Enter New line");
    }
    void draw() noexcept {
        locate();
        Paint paint{output_};
        paint.text("\x1b[H\x1b[2J");
        paint.text(" myos edit: ");
        const size_t length = service::length(path_);
        paint.append(path_, length < Columns - 12 ? length : Columns - 12);
        size_t start = line_start(top_);
        for (size_t r = 0; r < Rows; ++r) {
            row(paint, r, start);
            if (start <= buffer_.size()) {
                const size_t end = line_end(start);
                start = end < buffer_.size() ? end + 1 : buffer_.size() + 1;
            }
        }
        footer(paint); cursor(paint); paint.flush();
    }
    void draw_current() noexcept {
        const size_t old_top = top_, old_horizontal = horizontal_;
        locate();
        if (top_ != old_top || horizontal_ != old_horizontal) { draw(); return; }
        Paint paint{output_};
        row(paint, selected_ - top_, line_start(selected_));
        footer(paint); cursor(paint); paint.flush();
    }
    [[nodiscard]] auto key() noexcept -> int {
        const int first = input_.read();
        if (first != 27) return first;
        const int prefix = input_.read();
        if (prefix != '[' && prefix != 'O') return prefix;
        const int code = input_.read();
        if (code == 'A') return Up;
        if (code == 'B') return Down;
        if (code == 'C') return Right;
        if (code == 'D') return Left;
        if (code == 'H') return Home;
        if (code == 'F') return End;
        if (code == '3' || code == '5' || code == '6') {
            if (input_.read() != '~') return 0;
            return code == '3' ? Delete : code == '5' ? PageUp : PageDown;
        }
        return 0;
    }
    void vertical(bool down) noexcept {
        locate();
        if (down) {
            const size_t end = line_end(line_start(selected_));
            if (end == buffer_.size()) return;
            const size_t next = end + 1;
            const size_t last = line_end(next);
            buffer_.move(next + (column_ < last - next ? column_ : last - next));
        } else if (selected_ != 0) {
            const size_t previous = line_start(selected_ - 1);
            const size_t last = line_end(previous);
            buffer_.move(previous + (column_ < last - previous ? column_ : last - previous));
        }
    }
public:
    Screen(Buffer& buffer, myos_cap_t input, stream::Writer& output, const char* path) noexcept
        : buffer_(buffer), input_(input), output_(output), path_(path) {}

    template<class Save>
    [[nodiscard]] auto run(Save save) noexcept -> myos_status_t {
        output_.write("\x1b[?1049h\x1b[?25h");
        buffer_.move(0);
        draw();
        for (;;) {
            const int action = key();
            if (action == -1) { output_.write("\x1b[?1049l"); return MYOS_STATUS_CANCELED; }
            if (action == 15 || action == 19) {
                if (buffer_.modified()) {
                    const auto status = save(true);
                    if (status != MYOS_STATUS_OK) { output_.write("\x1b[?1049l"); return status; }
                    buffer_.clean();
                }
                notice_ = "Written";
                draw_current();
                continue;
            }
            if (action == 24) {
                if (buffer_.modified()) {
                    Paint paint{output_};
                    paint.position(24, 1); paint.text("\x1b[2KSave changes? (y/n/c) "); paint.flush();
                    const int answer = key();
                    if (answer == 'c' || answer == 'C') { draw(); continue; }
                    if (answer == 'y' || answer == 'Y') {
                        const auto status = save(false);
                        output_.write("\x1b[?1049l");
                        return status;
                    }
                    if (answer != 'n' && answer != 'N') { draw(); continue; }
                }
                output_.write("\x1b[?1049l");
                return MYOS_STATUS_OK;
            }
            bool full_redraw = action == 12;
            notice_ = nullptr;
            if (action == Left && buffer_.cursor() != 0) buffer_.move(buffer_.cursor() - 1);
            else if (action == Right && buffer_.cursor() < buffer_.size())
                buffer_.move(buffer_.cursor() + 1);
            else if (action == Up || action == Down) vertical(action == Down);
            else if (action == Home) buffer_.move(line_start(selected_));
            else if (action == End) buffer_.move(line_end(line_start(selected_)));
            else if (action == PageUp || action == PageDown)
                for (size_t i = 0; i < Rows; ++i) vertical(action == PageDown);
            else if (action == Delete) {
                if (buffer_.cursor() < buffer_.size()) {
                    full_redraw = buffer_.at(buffer_.cursor()) == '\n';
                    buffer_.erase();
                }
            } else if (action == '\b' || action == 127) {
                if (buffer_.cursor() != 0) {
                    full_redraw = buffer_.at(buffer_.cursor() - 1) == '\n';
                    buffer_.backspace();
                }
            }
            else if (action == '\n' || action == '\r') {
                if (!buffer_.insert('\n')) notice_ = "Buffer full";
                else full_redraw = true;
            } else if (action >= 32 && action < 127) {
                if (!buffer_.insert(static_cast<char>(action))) notice_ = "Buffer full";
            } else if (action != 12) continue;
            if (full_redraw) draw();
            else draw_current();
        }
    }
};

} // namespace myos::edit
