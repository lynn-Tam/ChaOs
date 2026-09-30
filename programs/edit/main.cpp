#include <unistd.h>
#include <sys/stat.h>
#include <libk/fmt.hpp>
#include <algorithm>
#include <string_view>
#include <programs/edit/screen.hpp>

namespace {
struct file {
    int fd;
    file(const char* path, unsigned flags) : fd(::open(path, flags)) {}
    explicit file(int value) : fd(value) {}
    file(const file&) = delete;
    ~file() { if (fd >= 0) ::close(fd); }
};

int send(int fd, const void* data, size_t size) {
    auto* bytes = static_cast<const char*>(data);
    while (size) {
        const auto count = write(fd, bytes, size);
        if (count <= 0) return count < 0 ? count : static_cast<int>(errc::io_error);
        bytes += count; size -= count;
    }
    return 0;
}
int copy(int source, int target) {
    char bytes[4096];
    for (;;) {
        const auto count = read(source, bytes, sizeof(bytes));
        if (count <= 0) return count;
        if (const int status = send(target, bytes, count)) return status;
    }
}

class writer {
    int fd_, status_{};
    char bytes_[4096];
    size_t used_{};
public:
    explicit writer(int fd) : fd_(fd) {}
    int flush() {
        if (!status_) status_ = send(fd_, bytes_, used_);
        used_ = 0;
        return status_;
    }
    void put(char byte) {
        if (status_) return;
        bytes_[used_++] = byte;
        if (used_ == sizeof(bytes_)) flush();
    }
    void line(const char* text) { while (*text) put(*text++); put('\n'); }
};

enum class change { append, insert, replace, erase };

class draft {
    const char* target_{};
    char paths_[2][20]{};
    unsigned active_{};
public:
    bool present() const { return paths_[0][0] || paths_[1][0]; }
    const char* path() const { return paths_[paths_[active_][0] ? active_ : 1 - active_]; }
    int open(const char* path) {
        target_ = path;
        active_ = 0;
        for (auto& name : paths_) {
            std::copy_n("/.edit-XXXXXX", 14, name);
            file temp{mkstemp(name)};
            if (temp.fd < 0) { name[0] = 0; return temp.fd; }
        }
        file source{target_, O_RDONLY};
        if (source.fd == errc::not_found) return 0;
        if (source.fd < 0) return source.fd;
        file dest{this->path(), O_WRONLY};
        return dest.fd < 0 ? dest.fd : copy(source.fd, dest.fd);
    }
    int show(edit::output& out) {
        file source{path(), O_RDONLY};
        if (source.fd < 0) return source.fd;
        char bytes[4096];
        uint64_t line = 1;
        bool start = true, any = false;
        for (;;) {
            const auto count = read(source.fd, bytes, sizeof(bytes));
            if (count < 0) return count;
            if (!count) break;
            any = true;
            for (int64_t i = 0; i < count; ++i) {
                if (start) (void)libk::fmt::format_to<"{}: ">(out, line);
                size_t end = i;
                while (end < static_cast<size_t>(count) && bytes[end] != '\n') ++end;
                start = end < static_cast<size_t>(count);
                if (start) { ++end; ++line; }
                out.write(bytes + i, end - i);
                i = end - 1;
            }
        }
        if (!any) out.write("(empty)\n");
        else if (!start) out.put('\n');
        return out.status();
    }
    int load(edit::buffer& text, bool& fits) {
        file source{path(), O_RDONLY};
        if (source.fd < 0) return source.fd;
        struct stat info{};
        if (const int status = fstat(source.fd, &info)) return status;
        fits = info.size <= edit::buffer::capacity;
        char bytes[4096];
        while (fits) {
            const auto count = read(source.fd, bytes, sizeof(bytes));
            if (count <= 0) return count;
            fits = text.append(reinterpret_cast<const uint8_t*>(bytes), count);
        }
        return 0;
    }
    int replace(const edit::buffer& text) {
        file dest{paths_[1 - active_], O_WRONLY | O_TRUNC};
        if (dest.fd < 0) return dest.fd;
        int status = send(dest.fd, text.before(), text.cursor());
        if (!status) status = send(dest.fd, text.after(), text.after_size());
        if (!status) status = fsync(dest.fd);
        if (!status) active_ = 1 - active_;
        return status;
    }
    int edit(change op, uint64_t target, const char* text) {
        file source{path(), O_RDONLY};
        if (source.fd < 0) return source.fd;
        file dest{paths_[1 - active_], O_WRONLY | O_TRUNC};
        if (dest.fd < 0) return dest.fd;
        writer out{dest.fd};
        char bytes[4096];
        uint64_t line = 1;
        bool start = true, done = false, skip = false, any = false;
        for (;;) {
            const auto count = read(source.fd, bytes, sizeof(bytes));
            if (count < 0) return count;
            if (!count) break;
            any = true;
            for (int64_t i = 0; i < count; ++i) {
                if (start) {
                    if (op == change::insert && line == target) { out.line(text); done = true; }
                    if ((op == change::replace || op == change::erase) && line == target) {
                        if (op == change::replace) out.line(text);
                        done = skip = true;
                    }
                    start = false;
                }
                if (!skip) out.put(bytes[i]);
                if (bytes[i] == '\n') { ++line; start = true; skip = false; }
            }
        }
        if (op == change::append) {
            if (any && !start) out.put('\n');
            out.line(text); done = true;
        } else if (op == change::insert && !done
            && target == line + (start ? 0 : 1)) {
            if (!start) out.put('\n');
            out.line(text); done = true;
        }
        if (!done) return errc::invalid;
        int status = out.flush();
        if (!status) status = fsync(dest.fd);
        if (!status) active_ = 1 - active_;
        return status;
    }
    int save() {
        int status = rename(path(), target_);
        if (!status) { paths_[active_][0] = 0; status = discard(); }
        return status;
    }
    int discard() {
        int status{};
        for (auto& path : paths_) if (path[0]) {
            const int removed = unlink(path);
            if (!removed) path[0] = 0;
            else if (!status) status = removed;
        }
        return status;
    }
};

// Canonical line editing is a UI choice here; the runtime owns stream transport.
int line_editor(draft& doc, edit::output& out) {
    out.write("Large file: line editor. Text or :a TEXT appends; :p shows, :i/:r/:d N edits; . saves, :q cancels.\n");
    for (;;) {
        out.write("edit> ");
        char text[128]{};
        size_t used{};
        bool overflow{};
        for (;;) {
            char byte;
            const auto count = read(STDIN_FILENO, &byte, 1);
            if (count < 0) return count;
            if (!count) return doc.discard();
            if (byte == '\n' || byte == '\r') { out.put('\n'); break; }
            if (byte == '\b' || byte == 127) {
                if (used) { --used; out.write("\b \b"); }
            } else if (byte >= 32 && byte < 127) {
                if (used + 1 < sizeof(text)) { text[used++] = byte; out.put(byte); }
                else overflow = true;
            }
        }
        if (out.status()) return out.status();
        if (overflow) { out.write("line too long\n"); continue; }
        const std::string_view command{text, used};
        if (command == ":q") return doc.discard();
        if (command == "." || command == ":w") return doc.save();
        int status{};
        if (command == ":p") status = doc.show(out);
        else if (command.starts_with(":a ")) status = doc.edit(change::append, 0, text + 3);
        else if (used >= 3 && text[0] == ':' && std::string_view("ird").find(text[1]) != std::string_view::npos && text[2] == ' ') {
            uint64_t line{};
            const char* end = text + 3;
            bool valid{};
            while (*end >= '0' && *end <= '9') {
                if (line > (UINT64_MAX - (*end - '0')) / 10) { valid = false; break; }
                line = line * 10 + (*end++ - '0');
                valid = true;
            }
            const bool erase = text[1] == 'd';
            if (!valid || !line
                || (erase ? end != text + used : end == text + used || *end != ' ')) {
                out.write("invalid line command\n"); continue;
            }
            status = doc.edit(erase ? change::erase : text[1] == 'i' ? change::insert : change::replace,
                line, erase ? "" : end + 1);
            if (status == errc::invalid) { out.write("line out of range\n"); continue; }
        } else if (command.starts_with(':') && !command.starts_with("::")) {
            out.write("invalid editor command\n"); continue;
        } else status = doc.edit(change::append, 0, text + (command.starts_with("::") ? 1 : 0));
        if (status) return status;
    }
}
edit::buffer text;
}

int main(int argc, char** argv) {
    if (argc != 2) return errc::invalid;
    edit::output out;
    draft doc;
    int status = doc.open(argv[1]);
    if (status) { if (doc.present()) doc.discard(); return status; }
    bool fits{};
    status = doc.load(text, fits);
    if (!status) {
        if (!fits) status = line_editor(doc, out);
        else {
            edit::screen screen{text, out, argv[1]};
            status = screen.run([&](bool again) {
                int saved = doc.replace(text);
                if (!saved) saved = doc.save();
                if (!saved && again) saved = doc.open(argv[1]);
                return saved;
            });
            if (!status && doc.present()) status = doc.discard();
        }
    }
    if (doc.present()) {
        out.write("draft: "); out.write(doc.path()); out.put('\n');
    }
    return status ? status : out.status();
}
