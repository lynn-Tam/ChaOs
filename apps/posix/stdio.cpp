#include <stdio.h>
#include <unistd.h>
#include <posix.hpp>
#include <sync.hpp>
#include <mutex>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <string.h>
#include <algorithm>
#include <vector>

struct px_file {
    _mutex lock;
    int fd;
    bool readable{}, writable{}, append{}, eof{}, error{}, standard{};
    int buffering{_IOFBF}, direction{}, pushed{EOF};
    uint64_t position{};
    size_t used{}, at{};
    unsigned char buffer[BUFSIZ]{};
    constexpr explicit px_file(int fd) : fd(fd) {}
};
static constinit px_file px_in{0}, px_out{1}, px_err{2};
extern "C" { FILE *stdin = &px_in, *stdout = &px_out, *stderr = &px_err; }
static _mutex px_stream_lock;
static std::vector<FILE*> px_streams;
static void px_standard() {
    static const bool initialized = [] {
        px_in.readable = px_out.writable = px_err.writable = true;
        px_in.standard = px_out.standard = px_err.standard = true;
        px_out.buffering = _IOLBF; px_err.buffering = _IONBF;
        if (std::atexit([] {
            (void)fflush(stdout); (void)fflush(stderr);
            for (;;) {
                FILE* f;
                { std::lock_guard lock{px_stream_lock}; if (px_streams.empty()) break; f = px_streams.back(); }
                (void)fclose(f);
            }
        })) _thread_exit(STATUS_NO_MEMORY);
        return true;
    }();
    (void)initialized;
}
static auto px_flush(FILE* f) -> int {
    if (f->direction == 2) while (f->used) {
        const auto n = f->standard ? write(f->fd, f->buffer, f->used)
            : pwrite(f->fd, f->buffer, f->used, f->position - f->used);
        if (n <= 0) { f->error = true; if (!n) errno = EIO; return EOF; }
        f->used -= n; ::memmove(f->buffer, f->buffer + n, f->used);
    }
    f->used = f->at = 0; return 0;
}
static auto px_mode(const char* mode, int& flags, bool& read, bool& write, bool& append) -> bool {
    if (!mode || !*mode) return false;
    read = *mode == 'r'; write = *mode == 'w' || *mode == 'a'; append = *mode == 'a';
    if (!read && !write) return false;
    flags = read ? O_RDONLY : O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
    bool plus{}, binary{}, exclusive{};
    for (++mode; *mode; ++mode) {
        if (*mode == '+' && !plus) plus = true;
        else if (*mode == 'b' && !binary) binary = true;
        else if (*mode == 'x' && !exclusive) exclusive = true;
        else return false;
    }
    if (plus) { read = write = true; flags = (flags & ~O_ACCMODE) | O_RDWR; }
    if (exclusive) flags |= O_EXCL;
    return true;
}
extern "C" FILE* fopen(const char* path, const char* mode) {
    px_standard();
    int flags; bool read, write, append;
    if (!px_mode(mode, flags, read, write, append)) { errno = EINVAL; return nullptr; }
    const int fd = open(path, flags, 0); if (fd < 0) return nullptr;
    auto* f = new (std::nothrow) FILE{fd}; if (!f) { close(fd); errno = ENOMEM; return nullptr; }
    f->readable = read; f->writable = write; f->append = append;
    if (append) { uint64_t size; if (px_size(fd, &size) == STATUS_OK) f->position = size; }
    std::lock_guard lock{px_stream_lock}; px_streams.push_back(f); return f;
}
extern "C" int fflush(FILE* f) {
    px_standard();
    if (!f) {
        int result{}; std::lock_guard lock{px_stream_lock};
        for (auto* stream : px_streams) { std::lock_guard file{stream->lock}; if (px_flush(stream)) result = EOF; }
        if (fflush(stdout) || fflush(stderr)) result = EOF;
        return result;
    }
    std::lock_guard lock{f->lock}; return px_flush(f);
}
extern "C" int fclose(FILE* f) {
    px_standard(); if (!f) { errno = EINVAL; return EOF; }
    int result;
    {
        std::lock_guard lock{f->lock}; result = px_flush(f);
        if (f->writable && !f->standard && fsync(f->fd) < 0) result = EOF;
        if (close(f->fd) < 0) result = EOF;
    }
    if (!f->standard) { std::lock_guard lock{px_stream_lock}; std::erase(px_streams, f); delete f; }
    return result;
}
extern "C" size_t fread(void* data, size_t size, size_t count, FILE* f) {
    px_standard(); if (!size || !count) return 0;
    if (count > SIZE_MAX / size) { errno = EOVERFLOW; return 0; }
    std::lock_guard lock{f->lock};
    if (!f->readable) { f->error = true; errno = EBADF; return 0; }
    if (f->direction == 2 && px_flush(f)) return 0;
    f->direction = 1;
    auto* dst = static_cast<unsigned char*>(data); size_t done{}, bytes = count * size;
    if (f->pushed != EOF) { dst[done++] = f->pushed; f->pushed = EOF; ++f->position; }
    while (done < bytes) {
        if (f->at == f->used) {
            if (!f->standard && bytes - done >= sizeof(f->buffer)) {
                f->used = f->at = 0;
                const auto n = pread(f->fd, dst + done, std::min(bytes - done, io_limit), f->position);
                if (n <= 0) { if (n < 0) f->error = true; else f->eof = true; break; }
                done += n; f->position += n; continue;
            }
            const auto n = f->standard ? read(f->fd, f->buffer, sizeof(f->buffer)) : pread(f->fd, f->buffer, sizeof(f->buffer), f->position);
            if (n <= 0) { if (n < 0) f->error = true; else f->eof = true; break; }
            f->at = 0; f->used = n;
        }
        const auto n = std::min(bytes - done, f->used - f->at);
        ::memcpy(dst + done, f->buffer + f->at, n); f->at += n; f->position += n; done += n;
    }
    return done / size;
}
extern "C" size_t fwrite(const void* data, size_t size, size_t count, FILE* f) {
    px_standard(); if (!size || !count) return 0;
    if (count > SIZE_MAX / size) { errno = EOVERFLOW; return 0; }
    std::lock_guard lock{f->lock};
    if (!f->writable) { f->error = true; errno = EBADF; return 0; }
    if (f->direction == 1) { f->used = f->at = 0; f->pushed = EOF; }
    if (f->append && !f->used) { uint64_t n; if (px_size(f->fd, &n) != STATUS_OK) { f->error = true; return 0; } f->position = n; }
    f->direction = 2;
    const auto* src = static_cast<const unsigned char*>(data); size_t done{}, bytes = count * size;
    while (done < bytes) {
        const auto n = std::min(bytes - done, sizeof(f->buffer) - f->used);
        ::memcpy(f->buffer + f->used, src + done, n); f->used += n; done += n; f->position += n;
        if (f->used == sizeof(f->buffer) || f->buffering == _IONBF || (f->buffering == _IOLBF && ::memchr(src + done - n, '\n', n))) if (px_flush(f)) break;
    }
    return done / size;
}
extern "C" int fseek(FILE* f, long offset, int whence) {
    px_standard(); std::lock_guard lock{f->lock};
    if (f->standard) { errno = ESPIPE; return -1; }
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) { errno = EINVAL; return -1; }
    if (f->direction == 2 && px_flush(f)) return -1;
    uint64_t base{};
    if (whence == SEEK_CUR) base = f->position;
    else if (whence == SEEK_END) { if (px_size(f->fd, &base) != STATUS_OK) { f->error = true; return -1; } }
    else if (whence != SEEK_SET) { errno = EINVAL; return -1; }
    const auto position = static_cast<__int128>(base) + offset;
    if (position < 0 || position > LONG_MAX) { errno = EINVAL; return -1; }
    const auto start = f->position + (f->pushed != EOF) - f->at;
    if (f->direction == 1 && uint64_t(position) >= start && uint64_t(position) - start <= f->used)
        f->at = uint64_t(position) - start;
    else f->used = f->at = 0;
    f->position = uint64_t(position); f->eof = false; f->pushed = EOF; return 0;
}
extern "C" long ftell(FILE* f) { px_standard(); std::lock_guard lock{f->lock}; if (f->position > LONG_MAX) { errno = EOVERFLOW; return -1; } return f->position; }
extern "C" int fgetpos(FILE* f, fpos_t* p) { const auto n = ftell(f); if (n < 0) return -1; *p = n; return 0; }
extern "C" int fsetpos(FILE* f, const fpos_t* p) { return fseek(f, *p, SEEK_SET); }
extern "C" void rewind(FILE* f) { (void)fseek(f, 0, SEEK_SET); clearerr(f); }
extern "C" int fgetc(FILE* f) { unsigned char c; return fread(&c, 1, 1, f) == 1 ? c : EOF; }
extern "C" int getc(FILE* f) { return fgetc(f); }
extern "C" int getchar() { return fgetc(stdin); }
extern "C" int ungetc(int c, FILE* f) { std::lock_guard lock{f->lock}; if (c == EOF || f->pushed != EOF || !f->position) return EOF; f->pushed = (unsigned char)c; --f->position; f->eof = false; return f->pushed; }
extern "C" char* fgets(char* data, int n, FILE* f) { if (n <= 0) return nullptr; int i = 0; while (i < n - 1) { auto c = fgetc(f); if (c == EOF) break; data[i++] = c; if (c == '\n') break; } data[i] = 0; return i || n == 1 ? data : nullptr; }
extern "C" int fputc(int c, FILE* f) { unsigned char v = c; return fwrite(&v, 1, 1, f) == 1 ? v : EOF; }
extern "C" int putc(int c, FILE* f) { return fputc(c, f); }
extern "C" int putchar(int c) { return fputc(c, stdout); }
extern "C" int fputs(const char* s, FILE* f) { const auto n = ::strlen(s); return fwrite(s, 1, n, f) == n ? 0 : EOF; }
extern "C" int puts(const char* s) { return fputs(s, stdout) == EOF ? EOF : fputc('\n', stdout); }
extern "C" int feof(FILE* f) { std::lock_guard lock{f->lock}; return f->eof; }
extern "C" int ferror(FILE* f) { std::lock_guard lock{f->lock}; return f->error; }
extern "C" void clearerr(FILE* f) { std::lock_guard lock{f->lock}; f->eof = f->error = false; }
extern "C" int fileno(FILE* f) { return f->fd; }
extern "C" int setvbuf(FILE* f, char* buffer, int mode, size_t) { if (buffer || (mode != _IOFBF && mode != _IOLBF && mode != _IONBF)) { errno = EINVAL; return -1; } std::lock_guard lock{f->lock}; if (px_flush(f)) return -1; f->buffering = mode; return 0; }
extern "C" void setbuf(FILE* f, char* buffer) { (void)setvbuf(f, buffer, buffer ? _IOFBF : _IONBF, BUFSIZ); }
extern "C" int remove(const char* path) { return unlink(path); }
extern "C" void perror(const char* text) { fprintf(stderr, "%s: error %d\n", text ? text : "", errno); }
