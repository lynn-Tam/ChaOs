#include <libk/fmt.hpp>
#include <user/lib/clock.hpp>
#include <user/lib/stream.hpp>
#include <user/lib/terminal.hpp>
#include <user/lib/vfs_client.hpp>
#include <user/lib/volume_path.hpp>

namespace {
using namespace myos;

enum class Edit { Append, Insert, Replace, Delete };

class Output final {
    vfs::Client& fs_;
    vfs::File file_;
    uint8_t buffer_[io::BufferSize]{};
    size_t used_{};
    uint64_t offset_{};
    myos_status_t status_{MYOS_STATUS_OK};
public:
    Output(vfs::Client& fs, vfs::File file) noexcept : fs_(fs), file_(file) {}
    void flush() noexcept {
        if (status_ == MYOS_STATUS_OK && used_ != 0) {
            status_ = fs_.write(file_, offset_, buffer_, used_);
            offset_ += used_;
            used_ = 0;
        }
    }
    void put(uint8_t byte) noexcept {
        if (status_ != MYOS_STATUS_OK) return;
        buffer_[used_++] = byte;
        if (used_ == sizeof(buffer_)) flush();
    }
    void line(const char* text) noexcept {
        while (*text != 0) put(*text++);
        put('\n');
    }
    [[nodiscard]] auto finish() noexcept -> myos_status_t { flush(); return status_; }
};

class Draft final {
    vfs::Client& fs_;
    char target_[sizeof(io::ControlMessage::data) + 1]{};
    char paths_[2][25]{};
    bool created_[2]{};
    size_t active_{};
public:
    explicit Draft(vfs::Client& fs) noexcept : fs_(fs) {}
    [[nodiscard]] auto present() const noexcept -> bool { return created_[0] || created_[1]; }
    [[nodiscard]] auto path() const noexcept -> const char* {
        return paths_[created_[active_] ? active_ : 1 - active_];
    }

    [[nodiscard]] auto open(const char* path) noexcept -> myos_status_t {
        const size_t length = service::length(path);
        if (length == 0 || length > sizeof(io::ControlMessage::data)) return MYOS_STATUS_BAD_ARGS;
        if (volume_path::boot_name(path) != nullptr) return MYOS_STATUS_DENIED;
        service::copy(target_, path, length + 1);
        const auto now = clock_now();
        if (now.status != MYOS_STATUS_OK) return now.status;
        uint64_t candidate = now.value;
        constexpr char digits[] = "0123456789abcdef";
        for (;;) {
            for (size_t slot = 0; slot < 2; ++slot) {
                service::copy(paths_[slot], "/.edit-", 7);
                for (size_t i = 0; i < 16; ++i)
                    paths_[slot][7 + i] = digits[(candidate >> ((15 - i) * 4)) & 15];
                paths_[slot][23] = slot == 0 ? 'a' : 'b';
                paths_[slot][24] = 0;
            }
            vfs::File first{};
            auto status = fs_.open(paths_[0], vfs::Write | vfs::Create | vfs::Exclusive, first);
            if (status == MYOS_STATUS_BUSY && candidate != UINT64_MAX) { ++candidate; continue; }
            if (status != MYOS_STATUS_OK) return status;
            created_[0] = true;
            status = fs_.close(first);
            if (status != MYOS_STATUS_OK) return status;
            vfs::File second{};
            status = fs_.open(paths_[1], vfs::Write | vfs::Create | vfs::Exclusive, second);
            if (status == MYOS_STATUS_BUSY && candidate != UINT64_MAX) {
                const auto removed = fs_.remove(paths_[0]);
                if (removed != MYOS_STATUS_OK) return removed;
                created_[0] = false;
                ++candidate;
                continue;
            }
            if (status != MYOS_STATUS_OK) return status;
            created_[1] = true;
            status = fs_.close(second);
            if (status != MYOS_STATUS_OK) return status;
            return copy_existing();
        }
    }

    [[nodiscard]] auto show(stream::Writer& output) noexcept -> myos_status_t {
        vfs::File file{};
        auto status = fs_.open(path(), vfs::Read, file);
        if (status != MYOS_STATUS_OK) return status;
        uint8_t buffer[io::BufferSize]{};
        uint64_t number = 1;
        bool start = true, newline = true;
        for (uint64_t offset = 0; offset < file.size && status == MYOS_STATUS_OK;) {
            const size_t length = file.size - offset < sizeof(buffer)
                ? file.size - offset : sizeof(buffer);
            uint64_t bytes{};
            status = fs_.read_at(file, offset, buffer, length, bytes);
            if (status == MYOS_STATUS_OK && bytes == 0) status = MYOS_STATUS_BACKING_FAILED;
            if (status != MYOS_STATUS_OK) break;
            size_t first{};
            for (size_t i = 0; i < bytes; ++i) if (buffer[i] == '\n') {
                if (start) (void)libk::fmt::format_to<"{}: ">(output, number);
                output.write(reinterpret_cast<const char*>(buffer + first), i + 1 - first);
                first = i + 1;
                ++number;
                start = true;
            }
            if (first < bytes) {
                if (start) (void)libk::fmt::format_to<"{}: ">(output, number);
                output.write(reinterpret_cast<const char*>(buffer + first), bytes - first);
                start = false;
            }
            newline = buffer[bytes - 1] == '\n';
            offset += bytes;
        }
        if (file.size == 0) output.write("(empty)\n");
        else if (!newline) output.put('\n');
        const auto closed = fs_.close(file);
        return status == MYOS_STATUS_OK ? closed : status;
    }

    [[nodiscard]] auto change(Edit edit, uint64_t target, const char* text) noexcept
        -> myos_status_t {
        vfs::File source{};
        auto status = fs_.open(path(), vfs::Read, source);
        if (status != MYOS_STATUS_OK) return status;
        vfs::File destination{};
        status = fs_.open(paths_[1 - active_], vfs::Write | vfs::Truncate, destination);
        if (status == MYOS_STATUS_OK) {
            Output output{fs_, destination};
            uint8_t buffer[io::BufferSize]{};
            uint64_t line = 1;
            bool start = true, done = false, skip = false, newline = true;
            for (uint64_t offset = 0; offset < source.size && status == MYOS_STATUS_OK;) {
                const size_t length = source.size - offset < sizeof(buffer)
                    ? source.size - offset : sizeof(buffer);
                uint64_t bytes{};
                status = fs_.read_at(source, offset, buffer, length, bytes);
                if (status == MYOS_STATUS_OK && bytes == 0) status = MYOS_STATUS_BACKING_FAILED;
                if (status != MYOS_STATUS_OK) break;
                for (size_t i = 0; i < bytes; ++i) {
                    if (start) {
                        if (edit == Edit::Insert && line == target) { output.line(text); done = true; }
                        if ((edit == Edit::Replace || edit == Edit::Delete) && line == target) {
                            if (edit == Edit::Replace) output.line(text);
                            done = skip = true;
                        }
                        start = false;
                    }
                    if (!skip) output.put(buffer[i]);
                    newline = buffer[i] == '\n';
                    if (newline) { ++line; start = true; skip = false; }
                }
                offset += bytes;
            }
            if (status == MYOS_STATUS_OK) {
                if (edit == Edit::Append) {
                    if (source.size != 0 && !newline) output.put('\n');
                    output.line(text);
                    done = true;
                } else if (edit == Edit::Insert && !done) {
                    if (target == line && start) { output.line(text); done = true; }
                    else if (target == line + 1 && !start) {
                        output.put('\n'); output.line(text); done = true;
                    }
                }
                status = done ? output.finish() : MYOS_STATUS_BAD_ARGS;
            }
            if (status == MYOS_STATUS_OK) status = fs_.sync(destination);
            const auto closed = fs_.close(destination);
            if (status == MYOS_STATUS_OK) status = closed;
        }
        const auto closed = fs_.close(source);
        if (status == MYOS_STATUS_OK) status = closed;
        if (status == MYOS_STATUS_OK) active_ = 1 - active_;
        return status;
    }

    [[nodiscard]] auto save() noexcept -> myos_status_t {
        auto status = fs_.rename(path(), target_);
        if (status == MYOS_STATUS_OK) {
            created_[active_] = false;
            active_ = 1 - active_;
            status = fs_.remove(path());
            if (status == MYOS_STATUS_OK) created_[active_] = false;
        }
        return status;
    }
    [[nodiscard]] auto discard() noexcept -> myos_status_t {
        myos_status_t status = MYOS_STATUS_OK;
        for (size_t i = 0; i < 2; ++i) if (created_[i]) {
            const auto removed = fs_.remove(paths_[i]);
            if (removed == MYOS_STATUS_OK) created_[i] = false;
            else if (status == MYOS_STATUS_OK) status = removed;
        }
        return status;
    }
private:
    [[nodiscard]] auto copy_existing() noexcept -> myos_status_t {
        vfs::File source{};
        auto status = fs_.open(target_, vfs::Read, source);
        if (status == MYOS_STATUS_NOT_FOUND) return MYOS_STATUS_OK;
        if (status != MYOS_STATUS_OK) return status;
        vfs::File target{};
        status = fs_.open(path(), vfs::Write | vfs::Truncate, target);
        if (status == MYOS_STATUS_OK) {
            uint8_t buffer[io::BufferSize]{};
            for (uint64_t offset = 0; offset < source.size && status == MYOS_STATUS_OK;) {
                const size_t length = source.size - offset < sizeof(buffer)
                    ? source.size - offset : sizeof(buffer);
                uint64_t bytes{};
                status = fs_.read_at(source, offset, buffer, length, bytes);
                if (status == MYOS_STATUS_OK && bytes == 0) status = MYOS_STATUS_BACKING_FAILED;
                if (status == MYOS_STATUS_OK) status = fs_.write(target, offset, buffer, bytes);
                offset += bytes;
            }
            if (status == MYOS_STATUS_OK) status = fs_.sync(target);
            const auto closed = fs_.close(target);
            if (status == MYOS_STATUS_OK) status = closed;
        }
        const auto closed = fs_.close(source);
        return status == MYOS_STATUS_OK ? closed : status;
    }
};

[[nodiscard]] auto number_and_text(char* input, uint64_t& number, const char*& text,
    bool needs_text) noexcept -> bool {
    char* end = input;
    while (*end >= '0' && *end <= '9') ++end;
    if (*end != 0 && *end != ' ') return false;
    const char separator = *end;
    *end = 0;
    const auto parsed = decimal(input);
    *end = separator;
    if (!parsed || *parsed == 0) return false;
    number = *parsed;
    text = separator == 0 ? end : end + 1;
    return needs_text ? separator == ' ' : separator == 0;
}
} // namespace

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    using namespace myos;
    const auto info = service::bootstrap(address, size);
    if (info.argument_count() != 2) exit(MYOS_STATUS_BAD_ARGS);
    const auto stdout = service::capability(info, bootstrap::imports::Stdout);
    stream::Writer output{stdout};
    vfs::Client fs;
    auto status = fs.connect(info);
    if (status != MYOS_STATUS_OK) exit(status);
    Draft draft{fs};
    status = draft.open(info.argument(1));
    if (status != MYOS_STATUS_OK) {
        if (draft.present()) (void)draft.discard();
        exit(status);
    }
    status = draft.show(output);
    if (status != MYOS_STATUS_OK) {
        output.write("draft: "); output.write(draft.path()); output.put('\n');
        exit(status);
    }
    output.write("Text or :a TEXT appends. :p shows, :i N TEXT inserts, :r N TEXT replaces, :d N deletes; . saves, :q cancels.\n");
    terminal::LineReader input{service::capability(info, bootstrap::imports::Stdin), stdout};
    char line[128]{};
    for (;;) {
        output.write("edit> ");
        const auto result = input.read(line);
        if (result == terminal::LineResult::TooLong) { output.write("line too long\n"); continue; }
        if (result == terminal::LineResult::End || service::equal(line, ":q")) {
            status = draft.discard(); break;
        }
        if (service::equal(line, ".") || service::equal(line, ":w")) {
            status = draft.save(); break;
        }
        if (service::equal(line, ":p")) status = draft.show(output);
        else if (line[0] == ':' && line[1] == 'a' && line[2] == ' ')
            status = draft.change(Edit::Append, 0, line + 3);
        else if (line[0] == ':' && (line[1] == 'i' || line[1] == 'r'
            || line[1] == 'd') && line[2] == ' ') {
            uint64_t number{};
            const char* text{};
            if (!number_and_text(line + 3, number, text, line[1] != 'd')) {
                output.write("invalid line command\n"); continue;
            }
            status = draft.change(line[1] == 'i' ? Edit::Insert
                : line[1] == 'r' ? Edit::Replace : Edit::Delete, number, text);
            if (status == MYOS_STATUS_BAD_ARGS) {
                output.write("line out of range\n"); continue;
            }
        } else if (line[0] == ':' && line[1] != ':') {
            output.write("invalid editor command\n"); continue;
        } else status = draft.change(Edit::Append, 0, line[0] == ':' ? line + 1 : line);
        if (status != MYOS_STATUS_OK) break;
    }
    if (draft.present()) {
        output.write("draft: "); output.write(draft.path()); output.put('\n');
    }
    const auto closed = fs.close();
    exit(status == MYOS_STATUS_OK ? closed : status);
}
