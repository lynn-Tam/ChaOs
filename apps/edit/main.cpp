#include <io.hpp>
#include <start.hpp>
#include <ubus/ubus.hpp>
#include <vm.hpp>
#include <deque>
#include <uapi/abi.h>
#include <libk/fmt.hpp>
#include <algorithm>
#include <string_view>
#include <string>
#include <libk/scope_guard.hpp>
#include <vector>

class ed_terminal {
    _ev_port* port_{};
    ubus_conn control_;
    io_tx output_;
    _ch_pair subscription_;
    ubus_rx events_, input_;
    uint64_t focus_{};
    bool focused_{};
    struct { uint16_t rows{24}, columns{80}; } size_;
    status_t status_{};
    std::deque<unsigned char> bytes_;
public:
    auto open(const _env& env, const ubus_msg& setup) -> status_t {
        port_ = &_events();
        auto status = control_.bind(setup.capability(setup.data["message"]["terminal"]));
        if (status == STATUS_OK) status = subscription_.open(env.alloc(), 8, 0);
        if (status == STATUS_OK) status = events_.open(subscription_.ends[0].selector(), port_);
        if (status == STATUS_OK) status = input_.open(setup.capability(setup.data["message"]["stdin"]), port_);
        if (status != STATUS_OK) return status;
        const auto sender = _channel_mint(subscription_.ends[1].selector(), env.cspace, 1, RIGHT_SEND | RIGHT_DUPLICATE);
        if (sender.status != STATUS_OK) return sender.status;
        ubus_msg request, response;
        const auto events_idx = request.offer(_cap_owner{sender.value}, RIGHT_SEND);
        if (!events_idx) return events_idx.error();
        request.data.cap("events", *events_idx);
        return control_.invoke("Fullscreen", std::move(request), response);
    }
    auto write(const char* data, size_t size) -> status_t {
        while (size) {
            io_frame frame; frame.size = std::min(size, io_stream_limit);
            ::memcpy(frame.data, data, frame.size);
            const auto status = output_.send(control_.endpoint(), frame, true, *control_.token("Frame"), focus_);
            if (status == STATUS_STALE) { focused_ = false; return STATUS_OK; }
            if (status != STATUS_OK) { status_ = status; return status; }
            data += frame.size; size -= frame.size;
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
                io_frame message;
                const auto received = io_recv(input_, message);
                if (received == STATUS_OK) {
                    if (message.status != STATUS_OK) return message.status;
                    if (!message.size) return STATUS_CANCELED;
                    for (size_t i = 0; i < message.size; ++i) bytes_.push_back(uint8_t(message.data[i]));
                    continue;
                }
                if (received != STATUS_WOULD_BLOCK) return received;
            }
            if (!block) return STATUS_WOULD_BLOCK;
            if (const auto wake = _ev_wait(port_, deadline); wake.status != STATUS_OK) return wake.status;
        }
    }
};

class ed_painter {
    ed_terminal& output_;
    libk::fmt::fixed_buffer<4096> bytes_;
public:
    explicit ed_painter(ed_terminal& output) noexcept : output_(output) {}
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
    using painter = ed_painter;

    ed_buffer& buffer_;
    ed_terminal& output_;
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
    ed_screen(ed_buffer& text, ed_terminal& out, const char* path) noexcept
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

class ed_file {
    ubus_conn document_;
    uint64_t next_{};
public:
    auto open(_cap_owner object) -> status_t { return document_.adopt(std::move(object)); }
    auto load(ed_buffer& text, ubus_conn& environment) -> status_t {
        ubus_msg response;
        auto status = document_.invoke("Stat", {}, response); if (status != STATUS_OK) return status;
        const auto size = response.data["size"].as<uint64_t>(); if (!size || *size > (UINT64_MAX - 2 * 1024 * 1024) / 4) return STATUS_BAD_ARGS;
        next_ = response.data["sequence"].as<uint64_t>().value_or(0);
        const auto need = (*size * 4 + 2 * 1024 * 1024 + 4095) & ~uint64_t{4095};
        status = _quota_info(_startup().quota).status; if (status != STATUS_OK) return status;
        const auto budget = *reinterpret_cast<const QuotaInfo*>(_ipc());
        if (need > budget.memory_limit) {
            ubus_msg request; request.data.put("bytes", need).put("caps", budget.cap_limit);
            status = environment.invoke("Resource", std::move(request), response); if (status != STATUS_OK) return status;
        }
        auto wired = _vm(); wired.pager = 0;
        auto buffer = _vm_area::create(_startup().alloc(), wired, 0, 4096); if (!buffer) return buffer.error();
        uint64_t offset{};
        for (;;) {
            ubus_msg request;
            const auto buffer_idx = request.attach(buffer->memory.selector(), RIGHT_MAP | RIGHT_DUPLICATE);
            if (!buffer_idx) return buffer_idx.error();
            request.data.put("id", ++next_).put("offset", offset)
                .put("buffer", ubus_buffer{*buffer_idx, 0, 4096});
            status = document_.invoke("Read", std::move(request), response); if (status != STATUS_OK) return status;
            const auto count = response.data["bytes"].as<uint64_t>(); if (!count || *count > 4096) return STATUS_PEER_FAULT;
            if (!*count) return STATUS_OK;
            text.append(reinterpret_cast<const uint8_t*>(buffer->address), *count); offset += *count;
        }
    }
    auto save(const ed_buffer& text) -> status_t {
        if (!document_.token("Save")) return STATUS_DENIED;
        const auto size = (std::max(text.size(), size_t{1}) + 4095) & ~size_t{4095};
        auto wired = _vm(); wired.pager = 0;
        auto buffer = _vm_area::create(_startup().alloc(), wired, 0, size); if (!buffer) return buffer.error();
        ::memcpy(reinterpret_cast<void*>(buffer->address), text.before(), text.cursor());
        ::memcpy(reinterpret_cast<void*>(buffer->address + text.cursor()), text.after(), text.after_size());
        ubus_msg request, response;
        const auto buffer_idx = request.attach(buffer->memory.selector(), RIGHT_MAP | RIGHT_DUPLICATE);
        if (!buffer_idx) return buffer_idx.error();
        request.data.put("buffer", ubus_buffer{*buffer_idx, 0, text.size()});
        return document_.invoke("Save", std::move(request), response);
    }
};

int main(int argc, char** argv) {
    const auto& env = _startup();
    if (argc != 2) return STATUS_BAD_ARGS;
    ubus_conn environment;
    auto status = environment.bind(env.bootstrap); if (status != STATUS_OK) return status;
    ubus_msg setup;
    status = environment.invoke("Receive", {}, setup); if (status != STATUS_OK) return status;
    ed_terminal terminal;
    status = terminal.open(env, setup); if (status != STATUS_OK) return status;
    uint64_t loan{};
    const auto cleanup = libk::on_scope_exit([&]() noexcept {
        if (loan) { ubus_msg request, response; request.data.put("action", "close").put("loan", loan);
            (void)environment.invoke("Resource", std::move(request), response); }
    });
    auto object = setup.take(setup.data["message"]["document"]);
    if (!object) {
        ubus_msg request, response; request.data.put("name", "files");
        status = environment.invoke("Get", std::move(request), response); if (status != STATUS_OK) return status;
        ubus_conn files; status = files.bind(response.capability(response.data["connector"])); if (status != STATUS_OK) return status;
        request = {}; request.data.put("action", "lend").put("bytes", uint64_t{192 * 1024}).put("caps", uint64_t{0})
            .put("kinds", word_t{OBJ_BIT(ObjChannel) | OBJ_BIT(ObjMem)});
        status = environment.invoke("Resource", std::move(request), response); if (status != STATUS_OK) return status;
        loan = *response.data["loan"].as<uint64_t>(); auto budget = response.take(response.data["quota"]);
        std::string path{argv[1]};
        if (!path.starts_with('/')) {
            std::string_view base{"/"};
            for (auto** entry = environ; entry && *entry; ++entry)
                if (std::string_view{*entry}.starts_with("PWD=")) base = std::string_view{*entry}.substr(4);
            path = std::string{base} + "/" + path;
        }
        request = {};
        const auto budget_idx = request.attach(budget.selector(), RIGHT_CREATE | RIGHT_INSPECT | RIGHT_DUPLICATE);
        if (!budget_idx) return budget_idx.error();
        request.data.put("path", path)
            .cap("budget", *budget_idx);
        status = files.invoke("Document", std::move(request), response); if (status != STATUS_OK) return status;
        object = response.take(response.data["handle"]);
    }
    ed_file document;
    status = document.open(std::move(object)); if (status != STATUS_OK) return status;
    ed_buffer text;
    status = document.load(text, environment);
    if (status == STATUS_OK) {
        ed_screen screen{text, terminal, argv[1]}; status = screen.run([&] { return document.save(text); });
    }
    return status ? status : terminal.status();
}
