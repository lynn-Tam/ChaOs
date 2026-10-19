#include <cerrno>
#include <posix.hpp>
#include <start.hpp>
#include <protocols/file.hpp>
#include <protocols/terminal.hpp>
#include <unistd.h>
#include <uapi/abi.h>
#include <libk/fmt.hpp>
#include <algorithm>
#include <string_view>
#include <string>
#include <libk/scope_guard.hpp>
#include <vector>

class ed_buffer final {
public:
    [[nodiscard]] auto size() const noexcept -> size_t { return left_ + data_.size() - right_; }
    [[nodiscard]] auto cursor() const noexcept -> size_t { return left_; }
    [[nodiscard]] auto modified() const noexcept -> bool { return modified_; }
    void clean() noexcept { modified_ = false; }
    [[nodiscard]] auto before() const noexcept -> const char* { return data_.data(); }
    [[nodiscard]] auto after() const noexcept -> const char* { return data_.data() + right_; }
    [[nodiscard]] auto after_size() const noexcept -> size_t { return data_.size() - right_; }
    [[nodiscard]] auto at(size_t position) const noexcept -> char {
        return data_[position < left_ ? position : right_ + position - left_];
    }
    void append(const uint8_t* data, size_t count) noexcept {
        grow(count);
        std::copy_n(data, count, data_.data() + left_);
        left_ += count;
    }
    void move(size_t position) noexcept {
        while (left_ > position) data_[--right_] = data_[--left_];
        while (left_ < position) data_[left_++] = data_[right_++];
    }
    void insert(char byte) noexcept {
        grow(1);
        data_[left_++] = byte;
        modified_ = true;
    }
    void backspace() noexcept { if (left_ != 0) { --left_; modified_ = true; } }
    void erase() noexcept { if (right_ != data_.size()) { ++right_; modified_ = true; } }
private:
    std::vector<char> data_;
    size_t left_{}, right_{};
    void grow(size_t count) {
        if (count <= right_ - left_) return;
        const auto old = data_.size(), tail = old - right_;
        data_.resize(std::max(old * 2, size() + count + 4096));
        std::copy_backward(data_.begin() + right_, data_.begin() + old, data_.end());
        right_ = data_.size() - tail;
    }
    bool modified_{};
};

class ed_screen final {
    auto rows() const noexcept -> size_t { return output_.rows() - 3; }
    auto columns() const noexcept -> size_t { return output_.columns(); }
    enum key_code : int { up = 256, down, left, right, home, end, page_up, page_down, del };
    using painter = tm_painter;

    ed_buffer& buffer_;
    tm_client& output_;
    const char* path_;
    size_t top_{};
    size_t selected_{};
    size_t column_{};
    size_t horizontal_{};
    std::string notice_;

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
        if (selected_ >= top_ + rows()) top_ = selected_ - rows() + 1;
        horizontal_ = column_ < columns() ? 0 : column_ - columns() + 1;
    }
    void cursor(painter& paint) const noexcept {
        paint.position(selected_ - top_ + 2, column_ - horizontal_ + 1);
    }
    void row(painter& paint, size_t row_number, size_t start) const noexcept {
        paint.position(row_number + 2, 1);
        paint.text("\x1b[2K");
        if (start > buffer_.size()) { paint.text("~"); return; }
        const size_t end = line_end(start);
        for (size_t i = start + horizontal_; i < end && i < start + horizontal_ + columns(); ++i) {
            const char byte = buffer_.at(i);
            const char shown = byte >= 32 && byte < 127 ? byte : ' ';
            paint.append(&shown, 1);
        }
    }
    void footer(painter& paint) const noexcept {
        paint.position(output_.rows() - 1, 1); paint.text("\x1b[2K");
        paint.text(buffer_.modified() ? "Modified  " : "Saved     ");
        const size_t length = std::string_view(path_).size();
        paint.append(path_, length < 48 ? length : 48);
        if (!notice_.empty()) { paint.text("  "); paint.text(notice_); }
        paint.position(output_.rows(), 1); paint.text("\x1b[2KCtrl+O Save   Ctrl+X Exit   Arrows Move   Enter New line");
    }
    void draw() noexcept {
        locate();
        painter paint{output_};
        paint.text("\x1b[H\x1b[2J");
        paint.text(" edit: ");
        const size_t length = std::string_view(path_).size();
        paint.append(path_, length < columns() - 12 ? length : columns() - 12);
        size_t start = line_start(top_);
        for (size_t r = 0; r < rows(); ++r) {
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
    auto get() noexcept -> int { return output_.read(); }
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
    ed_screen(ed_buffer& text, tm_client& out, const char* path) noexcept
        : buffer_(text), output_(out), path_(path) {}

    template<class Fn>
    [[nodiscard]] auto run(Fn save) noexcept -> int {
        if (const auto key = output_.read(); key < 0) return key;
        buffer_.move(0);
        draw();
        for (;;) {
            const int action = key();
            if (action < 0 || output_.status()) {
                return action < 0 ? action : output_.status();
            }
            if (action == 15 || action == 19) {
                status_t status{};
                if (buffer_.modified()) {
                    status = save();
                    if (!status) buffer_.clean();
                }
                notice_ = status ? "Save failed: " + std::to_string(status) : "Written";
                draw_current();
                continue;
            }
            if (action == 24) {
                if (buffer_.modified()) {
                    painter paint{output_};
                    paint.position(output_.rows(), 1); paint.text("\x1b[2KSave changes? (y/n/c) "); paint.flush();
                    const int answer = key();
                    if (answer == 'c' || answer == 'C') { draw(); continue; }
                    if (answer == 'y' || answer == 'Y') {
                        const auto status = save();
                        if (!status) return 0;
                        notice_ = "Save failed: " + std::to_string(status);
                        draw_current(); continue;
                    }
                    if (answer != 'n' && answer != 'N') { draw(); continue; }
                }
                return 0;
            }
            bool full_redraw = action == 12;
            notice_.clear();
            if (action == left && buffer_.cursor() != 0) buffer_.move(buffer_.cursor() - 1);
            else if (action == right && buffer_.cursor() < buffer_.size())
                buffer_.move(buffer_.cursor() + 1);
            else if (action == up || action == down) vertical(action == down);
            else if (action == home) buffer_.move(line_start(selected_));
            else if (action == end) buffer_.move(line_end(line_start(selected_)));
            else if (action == page_up || action == page_down)
                for (size_t i = 0; i < rows(); ++i) vertical(action == page_down);
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
                buffer_.insert('\n'); full_redraw = true;
            } else if (action >= 32 && action < 127) {
                buffer_.insert(static_cast<char>(action));
            } else if (action != 12) continue;
            if (full_redraw) draw();
            else draw_current();
        }
    }
};

struct file {
    int fd;
    file(const char* path, unsigned flags) : fd(::open(path, flags)) {}
    explicit file(int value) : fd(value) {}
    file(const file&) = delete;
    ~file() { if (fd >= 0) ::close(fd); }
};

static int send(int fd, const void* data, size_t size) {
    auto* bytes = static_cast<const char*>(data);
    while (size) {
        const auto count = write(fd, bytes, size);
        if (count <= 0) return count < 0 ? count : static_cast<int>(STATUS_BACKING_FAILED);
        bytes += count; size -= count;
    }
    return 0;
}
class ed_file {
    const char* target_;
public:
    explicit ed_file(const char* path) : target_(path) {}
    int load(ed_buffer& text) {
        file source{target_, O_RDONLY};
        if (source.fd < 0 && errno == ENOENT) return 0;
        if (source.fd < 0) return source.fd;
        uint8_t bytes[4096];
        for (;;) {
            const auto count = read(source.fd, bytes, sizeof(bytes));
            if (count <= 0) return count;
            text.append(bytes, count);
        }
    }
    int save(const ed_buffer& text) {
        std::string path{target_};
        const auto slash = path.rfind('/');
        path.replace(slash == path.npos ? 0 : slash + 1, path.npos, ".edit-XXXXXX");
        file dest{mkstemp(path.data())};
        if (dest.fd < 0) return dest.fd;
        libk::scope_exit discard{[&]() noexcept { (void)unlink(path.c_str()); }};
        int status = send(dest.fd, text.before(), text.cursor());
        if (!status) status = send(dest.fd, text.after(), text.after_size());
        if (!status) status = fsync(dest.fd);
        return status ? status : rename(path.c_str(), target_);
    }
};

int main(int argc, char** argv) {
    const auto& env = rt_startup();
    auto directory = env.selector(vfs_service);
    if (!directory) directory = env.selector(vfs_read);
    if (const auto rc = px_init(env.alloc(), env.vspace, env.events, directory,
        {env.selector(rt_stdin), env.selector(rt_stdout), env.selector(rt_stderr)}); rc != STATUS_OK) return rc;

    if (argc != 2) return STATUS_BAD_ARGS;
    tm_client terminal;
    if (const auto status = terminal.open(env); status != STATUS_OK) return status;
    auto& out = terminal;
    ed_file doc{argv[1]};
    ed_buffer text;
    int status = doc.load(text);
    if (!status) {
        ed_screen screen{text, out, argv[1]};
        status = screen.run([&] { return doc.save(text); });
    }
    return status ? status : out.status();
}
