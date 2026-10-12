#pragma once

#include <unistd.h>
#include <sys/status.h>
#include <algorithm>
#include <string_view>

namespace edit {

class output {
    int status_{};
public:
    void write(const char* data, size_t size) noexcept {
        while (!status_ && size) {
            const auto count = ::write(STDOUT_FILENO, data, size);
            if (count <= 0) { status_ = count < 0 ? count : static_cast<int>(errc::io_error); break; }
            data += count; size -= count;
        }
    }
    void write(std::string_view text) noexcept { write(text.data(), text.size()); }
    void put(char byte) noexcept { write(&byte, 1); }
    int status() const noexcept { return status_; }
};

class buffer final {
public:
    static constexpr size_t capacity = 128 * 1024;
    [[nodiscard]] auto size() const noexcept -> size_t { return left_ + capacity - right_; }
    [[nodiscard]] auto cursor() const noexcept -> size_t { return left_; }
    [[nodiscard]] auto modified() const noexcept -> bool { return modified_; }
    void clean() noexcept { modified_ = false; }
    [[nodiscard]] auto before() const noexcept -> const char* { return data_; }
    [[nodiscard]] auto after() const noexcept -> const char* { return data_ + right_; }
    [[nodiscard]] auto after_size() const noexcept -> size_t { return capacity - right_; }
    [[nodiscard]] auto at(size_t position) const noexcept -> char {
        return data_[position < left_ ? position : right_ + position - left_];
    }
    [[nodiscard]] auto append(const uint8_t* data, size_t count) noexcept -> bool {
        if (count > right_ - left_) return false;
        std::copy_n(data, count, data_ + left_);
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
    void erase() noexcept { if (right_ != capacity) { ++right_; modified_ = true; } }
private:
    char data_[capacity]{};
    size_t left_{}, right_{capacity};
    bool modified_{};
};

class screen final {
    static constexpr size_t rows = 21, columns = 80;
    enum key_code : int { up = 256, down, left, right, home, end, page_up, page_down, del };
    class painter final {
        output& output_;
        char bytes_[4096]{};
        size_t used_{};
    public:
        explicit painter(output& output) noexcept : output_(output) {}
        void flush() noexcept {
            if (used_ != 0) { output_.write(bytes_, used_); used_ = 0; }
        }
        void append(const char* data, size_t size) noexcept {
            while (size != 0) {
                if (used_ == sizeof(bytes_)) flush();
                const size_t count = size < sizeof(bytes_) - used_ ? size : sizeof(bytes_) - used_;
                std::copy_n(data, count, bytes_ + used_);
                used_ += count; data += count; size -= count;
            }
        }
        void text(const char* value) noexcept { append(value, std::string_view(value).size()); }
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

    buffer& buffer_;
    output& output_;
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
        if (selected_ >= top_ + rows) top_ = selected_ - rows + 1;
        horizontal_ = column_ < columns ? 0 : column_ - columns + 1;
    }
    void cursor(painter& paint) const noexcept {
        paint.position(selected_ - top_ + 2, column_ - horizontal_ + 1);
    }
    void row(painter& paint, size_t row_number, size_t start) const noexcept {
        paint.position(row_number + 2, 1);
        paint.text("\x1b[2K");
        if (start > buffer_.size()) { paint.text("~"); return; }
        const size_t end = line_end(start);
        for (size_t i = start + horizontal_; i < end && i < start + horizontal_ + columns; ++i) {
            const char byte = buffer_.at(i);
            const char shown = byte >= 32 && byte < 127 ? byte : ' ';
            paint.append(&shown, 1);
        }
    }
    void footer(painter& paint) const noexcept {
        paint.position(23, 1); paint.text("\x1b[2K");
        paint.text(buffer_.modified() ? "Modified  " : "Saved     ");
        const size_t length = std::string_view(path_).size();
        paint.append(path_, length < 48 ? length : 48);
        if (notice_) { paint.text("  "); paint.text(notice_); }
        paint.position(24, 1); paint.text("\x1b[2KCtrl+O Save   Ctrl+X Exit   Arrows Move   Enter New line");
    }
    void draw() noexcept {
        locate();
        painter paint{output_};
        paint.text("\x1b[H\x1b[2J");
        paint.text(" edit: ");
        const size_t length = std::string_view(path_).size();
        paint.append(path_, length < columns - 12 ? length : columns - 12);
        size_t start = line_start(top_);
        for (size_t r = 0; r < rows; ++r) {
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
        painter paint{output_};
        row(paint, selected_ - top_, line_start(selected_));
        footer(paint); cursor(paint); paint.flush();
    }
    static int get() noexcept {
        char byte;
        const auto count = ::read(STDIN_FILENO, &byte, 1);
        return count == 1 ? static_cast<unsigned char>(byte)
            : count < 0 ? count : static_cast<int>(errc::canceled);
    }
    [[nodiscard]] auto key() noexcept -> int {
        const int first = get();
        if (first != 27) return first;
        const int prefix = get();
        if (prefix != '[' && prefix != 'O') return prefix;
        const int code = get();
        if (code == 'A') return up;
        if (code == 'B') return down;
        if (code == 'C') return right;
        if (code == 'D') return left;
        if (code == 'H') return home;
        if (code == 'F') return end;
        if (code == '3' || code == '5' || code == '6') {
            if (get() != '~') return 0;
            return code == '3' ? del : code == '5' ? page_up : page_down;
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
    screen(buffer& text, output& out, const char* path) noexcept
        : buffer_(text), output_(out), path_(path) {}

    template<class Fn>
    [[nodiscard]] auto run(Fn save) noexcept -> int {
        output_.write("\x1b[?1049h\x1b[?25h");
        buffer_.move(0);
        draw();
        for (;;) {
            const int action = key();
            if (action < 0 || output_.status()) {
                output_.write("\x1b[?1049l");
                return action < 0 ? action : output_.status();
            }
            if (action == 15 || action == 19) {
                if (buffer_.modified()) {
                    const auto status = save(true);
                    if (status != 0) { output_.write("\x1b[?1049l"); return status; }
                    buffer_.clean();
                }
                notice_ = "Written";
                draw_current();
                continue;
            }
            if (action == 24) {
                if (buffer_.modified()) {
                    painter paint{output_};
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
                return 0;
            }
            bool full_redraw = action == 12;
            notice_ = nullptr;
            if (action == left && buffer_.cursor() != 0) buffer_.move(buffer_.cursor() - 1);
            else if (action == right && buffer_.cursor() < buffer_.size())
                buffer_.move(buffer_.cursor() + 1);
            else if (action == up || action == down) vertical(action == down);
            else if (action == home) buffer_.move(line_start(selected_));
            else if (action == end) buffer_.move(line_end(line_start(selected_)));
            else if (action == page_up || action == page_down)
                for (size_t i = 0; i < rows; ++i) vertical(action == page_down);
            else if (action == del) {
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
                if (!buffer_.insert('\n')) notice_ = "buffer full";
                else full_redraw = true;
            } else if (action >= 32 && action < 127) {
                if (!buffer_.insert(static_cast<char>(action))) notice_ = "buffer full";
            } else if (action != 12) continue;
            if (full_redraw) draw();
            else draw_current();
        }
    }
};

} // namespace edit
