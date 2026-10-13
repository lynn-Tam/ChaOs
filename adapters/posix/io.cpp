#include <string.h>
#include <utility>
#include <variant>
#include <array>
#include <deque>
#include <vector>
#include <memory>
#include <cerrno>
#include <fcntl.h>
#include <cstdarg>
#include <unistd.h>
#include <dirent.h>
#include <posix.hpp>
#include <time.h>
#include <libk/checked_arithmetic.hpp>

#include <ipc.hpp>
#include <protocols/file.hpp>
#include <protocols/stream.hpp>

// This adapter currently supports one user thread; errno is not TLS yet.
extern "C" auto __errno() -> int* { static int value; return &value; }
static auto px_result(int64_t value) noexcept -> int64_t {
    if (value >= 0) return value;
    switch (value) {
    case STATUS_INVALID_CAP: errno = EBADF; break;
    case STATUS_INVALID_OP: errno = ENOTSUP; break;
    case STATUS_BAD_RIGHTS: case STATUS_DENIED: errno = EACCES; break;
    case STATUS_BAD_ARGS: errno = EINVAL; break;
    case STATUS_NOT_FOUND: errno = ENOENT; break;
    case STATUS_BUSY: errno = EBUSY; break;
    case STATUS_NO_MEMORY: errno = ENOMEM; break;
    case STATUS_WOULD_BLOCK: errno = EAGAIN; break;
    case STATUS_CANCELED: errno = ECANCELED; break;
    case STATUS_TIMED_OUT: errno = ETIMEDOUT; break;
    case STATUS_CLOSED: case STATUS_PEER_CLOSED: errno = EPIPE; break;
    case file_exists: errno = EEXIST; break;
    case file_not_dir: errno = ENOTDIR; break;
    case file_is_dir: errno = EISDIR; break;
    case file_not_empty: errno = ENOTEMPTY; break;
    case file_name_long: errno = ENAMETOOLONG; break;
    case file_too_large: errno = EFBIG; break;
    case file_no_space: errno = ENOSPC; break;
    default: errno = EIO; break;
    }
    return -1;
}

struct directory {
    ch_msg batch{};
    char path[sizeof(ch_msg::data) + 1]{};
    uint64_t pos{};
    bool end{};
    dirent entry{};
};


struct input_stream {
    cap_owner channel;
    str_reader reader;
    ch_msg frame{};
    uint64_t offset{};
};
struct output_stream { cap_owner channel; };
struct open_desc {
    std::variant<std::monostate, input_stream, output_stream, io_client> data;
    int refs{}; // fd references and pending completions share this owner.
    auto queue() noexcept -> io_client& { return std::get<io_client>(data); }
};
static std::deque<open_desc> opened{3};
static std::vector<open_desc*> fds(3);
static ch_rpc vfs;
static bool connected{};
static cap_t vspace{}, events{};
static cap_t vfs_cap{};
static cap_t cspace{};
static cap_t pool{};
static cap_owner timer;
static std::deque<std::unique_ptr<directory>> dirs;
static auto desc(int fd) noexcept -> open_desc* {
    return fd >= 0 && static_cast<size_t>(fd) < fds.size() ? fds[fd] : nullptr;
}
static auto file(int fd) noexcept -> open_desc* {
    auto* value = desc(fd);
    return value && std::holds_alternative<io_client>(value->data) ? value : nullptr;
}
static auto connect_vfs() noexcept -> status_t {
    if (connected) return STATUS_OK;
    if (!vfs_cap) return STATUS_INVALID_CAP;
    const auto status = vfs.open(vfs_cap, pool, cspace, events);
    if (status == STATUS_OK) connected = true;
    return status;
}
struct pending {
    open_desc* file{};
    request_id id{}, wire{};
    int64_t result{};
    bool ready{}, internal{};
};
static_assert(IO_DEPTH == io_queue_depth && IO_LIMIT == io_buffer_size);
pending requests[IO_DEPTH];
static request_id next_id{};
static auto release(open_desc& value) noexcept -> int32_t {
    if (value.refs > 1) { --value.refs; return STATUS_OK; }
    status_t status{};
    if (auto* input = std::get_if<input_stream>(&value.data)) {
        status = input->reader.close();
        if (status != STATUS_OK) return status;
        status = input->channel.close();
        if (status != STATUS_OK) return status;
    } else if (auto* output = std::get_if<output_stream>(&value.data)) {
        status = output->channel.close();
        if (status != STATUS_OK) return status;
    } else status = value.queue().close();
    value.data.emplace<std::monostate>();
    value.refs = 0;
    return status;
}
static auto enqueue(open_desc& file, io_operation op, void* data, uint64_t size,
    uint64_t offset, bool implicit, bool internal, pending*& out) noexcept -> int32_t {
    if (next_id == UINT64_MAX || (size && !data)) return STATUS_BAD_ARGS;
    for (auto& item : requests) if (!item.file) {
        io_cmd request{.operation = static_cast<uint64_t>(op),
            .offset = offset, .length = size, .flags = implicit ? 1u : 0u};
        const auto submitted = file.queue().submit(request, data);
        if (submitted != STATUS_OK) return submitted;
        item = {.file = &file, .id = ++next_id, .wire = request.id, .internal = internal};
        ++file.refs;
        out = &item;
        return STATUS_OK;
    }
    return STATUS_BUSY;
}
static void pump() noexcept {
    for (auto& file : opened) if (std::holds_alternative<io_client>(file.data)) {
        io_done result{};
        while (file.queue().completion(result) == STATUS_OK) {
            for (auto& item : requests) if (item.file == &file && item.wire == result.id) {
                item.result = result.status == STATUS_OK
                    ? static_cast<int64_t>(result.bytes) : result.status;
                item.ready = true;
                break;
            }
        }
    }
}
static auto wait() noexcept -> int32_t {
    for (auto& file : opened) if (std::holds_alternative<io_client>(file.data) && !file.queue().requests().error()) {
        const auto status = file.queue().arm();
        if (status != STATUS_OK) {
            (void)file.queue().requests().fail(status);
            return STATUS_OK;
        }
    }
    const auto status = sys::notification_wait(events).status;
    if (status != STATUS_OK)
        for (auto& file : opened) if (std::holds_alternative<io_client>(file.data)) (void)file.queue().requests().fail(status);
    return status;
}
static auto take(pending& item) noexcept -> int64_t {
    const auto result = item.result;
    auto* file = item.file;
    item = {};
    (void)release(*file);
    return result;
}
static auto drain(open_desc* target = nullptr) noexcept -> int32_t {
    for (;;) {
        pump();
        bool active{};
        for (const auto& item : requests)
            if (item.file && (!target || item.file == target) && !item.ready) active = true;
        if (!active) break;
        const auto status = wait();
        if (status != STATUS_OK) return status;
    }
    return STATUS_OK;
}
static auto finish(pending& item) noexcept -> int64_t {
    while (!item.ready) {
        pump();
        if (item.ready) break;
        const auto status = wait();
        if (status != STATUS_OK) (void)item.file->queue().requests().fail(status);
    }
    return take(item);
}
static auto transfer(open_desc& file, io_operation op, void* data, uint64_t size,
    uint64_t offset, bool explicit_offset) noexcept -> int64_t {
    if (size > INT64_MAX || (size && !data)
        || (explicit_offset && offset > UINT64_MAX - size)) return STATUS_BAD_ARGS;
    uint64_t done{};
    while (done < size) {
        const auto count = size - done < IO_LIMIT ? size - done : uint64_t{IO_LIMIT};
        pending* item{};
        const auto status = enqueue(file, op, static_cast<uint8_t*>(data) + done,
            count, offset + done, !explicit_offset, true, item);
        if (status != STATUS_OK) return done ? static_cast<int64_t>(done) : status;
        const auto result = finish(*item);
        if (result < 0) return done ? static_cast<int64_t>(done) : result;
        done += result;
        if (static_cast<uint64_t>(result) < count) break;
    }
    return done;
}




extern "C" DIR* opendir(const char* path) {
    if (!path || ::strlen(path) > sizeof(ch_msg::data)) { errno = EINVAL; return nullptr; }
    auto status = connect_vfs();
    if (status != STATUS_OK) { (void)px_result(status); return nullptr; }
    auto dir = std::make_unique<directory>();
    ::memcpy(dir->path, path, ::strlen(path) + 1);
    status = st_list(vfs, dir->batch, dir->path);
    if (status != STATUS_OK) { (void)px_result(status); return nullptr; }
    auto* result = dir.get();
    for (auto& slot : dirs) if (!slot) { slot = std::move(dir); return result; }
    dirs.push_back(std::move(dir));
    return result;
}

extern "C" dirent* readdir(DIR* dir) {
    if (std::ranges::none_of(dirs, [dir](const auto& slot) { return slot && slot.get() == dir; })) {
        errno = EBADF; return nullptr;
    }
    while (dir->pos == dir->batch.size) {
        if (dir->end || !dir->batch.value) { dir->end = true; return nullptr; }
        dir->batch = {.value = dir->batch.value};
        const auto status = st_list(vfs, dir->batch, dir->path);
        if (status != STATUS_OK) { (void)px_result(status); return nullptr; }
        dir->pos = 0;
    }
    size_t size{};
    while (dir->pos < dir->batch.size && dir->batch.data[dir->pos])
        dir->entry.d_name[size++] = dir->batch.data[dir->pos++];
    dir->entry.d_name[size] = '\0';
    if (dir->pos < dir->batch.size) ++dir->pos;
    return &dir->entry;
}

extern "C" int closedir(DIR* dir) {
    for (auto& slot : dirs) if (slot && slot.get() == dir) { slot.reset(); return 0; }
    errno = EBADF; return -1;
}

auto px_init(cap_t alloc, cap_t vm, cap_t space, cap_t wake,
             cap_t directory, std::array<cap_t, 3> streams) noexcept -> status_t {
    if (pool || !alloc || !vm || !space || !wake) return STATUS_BAD_ARGS;
    pool = alloc; vspace = vm; cspace = space; events = wake; vfs_cap = directory;
    for (int fd = 0; fd < 3; ++fd) {
        if (!streams[fd]) continue;
        auto& value = opened[fd];
        if (fd == 0) value.data.emplace<input_stream>().channel = cap_owner{streams[fd]};
        else value.data.emplace<output_stream>().channel = cap_owner{streams[fd]};
        value.refs = 1;
        fds[fd] = &value;
    }
    return STATUS_OK;
}

extern "C" int32_t clock_gettime(int32_t clock, timespec* time) {
    if (clock != CLOCK_MONOTONIC || !time) return px_result(STATUS_BAD_ARGS);
    const auto frequency = sys::clock_frequency();
    if (frequency.status != STATUS_OK) return px_result(frequency.status);
    const auto now = sys::clock_now();
    if (now.status != STATUS_OK) return px_result(now.status);
    const auto fraction = libk::checked_multiply(now.value % frequency.value, uint64_t{1'000'000'000});
    if (!fraction || now.value / frequency.value > INT64_MAX) return px_result(STATUS_BAD_ARGS);
    *time = {static_cast<int64_t>(now.value / frequency.value),
        static_cast<int64_t>(*fraction / frequency.value)};
    return px_result(STATUS_OK);
}

extern "C" int32_t nanosleep(const timespec* delay, timespec* remaining) {
    if (!delay || delay->tv_sec < 0 || delay->tv_nsec < 0
        || delay->tv_nsec >= 1'000'000'000) return px_result(STATUS_BAD_ARGS);
    const auto frequency = sys::clock_frequency();
    if (frequency.status != STATUS_OK) return px_result(frequency.status);
    const auto seconds = libk::checked_multiply(static_cast<uint64_t>(delay->tv_sec), frequency.value);
    const auto fraction = libk::checked_multiply(static_cast<uint64_t>(delay->tv_nsec), frequency.value);
    if (!seconds || !fraction) return px_result(STATUS_BAD_ARGS);
    const auto ticks = libk::checked_add(*seconds,
        *fraction / 1'000'000'000 + (*fraction % 1'000'000'000 != 0));
    if (!ticks) return px_result(STATUS_BAD_ARGS);
    const auto now = sys::clock_now();
    if (now.status != STATUS_OK) return px_result(now.status);
    const auto deadline = libk::checked_add(now.value, *ticks);
    if (!deadline) return px_result(STATUS_BAD_ARGS);
    if (*ticks == 0) { if (remaining) *remaining = {}; return px_result(STATUS_OK); }
    if (!timer) {
        const auto created = sys::notification_create(pool, 1);
        if (created.status != STATUS_OK) return px_result(created.status);
        timer = cap_owner{created.value};
    }
    const auto status = sys::notification_wait(timer.selector(), *deadline).status;
    if (status == STATUS_TIMED_OUT) {
        if (remaining) *remaining = {};
        return px_result(STATUS_OK);
    }
    if (remaining) {
        const auto end = sys::clock_now();
        if (end.status == STATUS_OK) {
            const auto left = end.value < *deadline ? *deadline - end.value : 0;
            const auto ns = libk::checked_multiply(left % frequency.value, uint64_t{1'000'000'000});
            if (ns) *remaining = {static_cast<int64_t>(left / frequency.value),
                static_cast<int64_t>(*ns / frequency.value)};
        }
    }
    return px_result(status);
}

extern "C" int open(const char* path, int flags, ...) {
    if (!path || !vfs_cap || (flags & ~(3u | O_CREAT | O_TRUNC | O_EXCL | O_APPEND)) != 0
        || (flags & 3u) == 3u) return px_result(STATUS_BAD_ARGS);
    if (flags & O_CREAT) {
        va_list args; va_start(args, flags);
        const auto mode = va_arg(args, int); va_end(args);
        if (mode != 0) { errno = ENOTSUP; return -1; }
    }
    const uint32_t mode = flags & 3u;
    if (mode == O_RDONLY && (flags & (O_CREAT | O_TRUNC | O_EXCL | O_APPEND)))
        return px_result(STATUS_BAD_ARGS);
    size_t fd{}, slot{};
    while (fd < fds.size() && fds[fd]) ++fd;
    if (fd == fds.size()) fds.push_back(nullptr);
    while (slot < opened.size() && !std::holds_alternative<std::monostate>(opened[slot].data)) ++slot;
    if (slot == opened.size()) opened.emplace_back();
    const auto connected_status = connect_vfs();
    if (connected_status != STATUS_OK) return px_result(connected_status);
    uint64_t backend = mode == O_RDONLY ? st_read
        : mode == O_WRONLY ? st_write
        : st_read | st_write;
    if (flags & O_CREAT) backend |= st_create;
    if (flags & O_TRUNC) backend |= st_truncate;
    if (flags & O_EXCL) backend |= st_exclusive;
    if (flags & O_APPEND) backend |= st_append;
    cap_owner object;
    auto status = st_open(vfs, path, backend, object);
    if (status != STATUS_OK) return px_result(status);
    uint64_t value{};
    auto& session = opened[slot].data.emplace<io_client>();
    status = session.adopt(std::move(object), events, vspace,
        0, value, mode != O_RDONLY);
    if (status != STATUS_OK) { opened[slot].data.emplace<std::monostate>(); return px_result(status); }
    opened[slot].refs = 1;
    fds[fd] = &opened[slot];
    return px_result(fd);
}

extern "C" int32_t close(int32_t fd) {
    auto* value = desc(fd);
    if (!value) return px_result(STATUS_INVALID_CAP);
    const auto status = release(*value);
    if (status == STATUS_OK || std::holds_alternative<std::monostate>(value->data)) fds[fd] = nullptr;
    return px_result(status);
}

extern "C" int32_t mkstemp(char* path) {
    if (!path) return px_result(STATUS_BAD_ARGS);
    const auto size = ::strlen(path);
    if (size < 6) return px_result(STATUS_BAD_ARGS);
    for (size_t i = size - 6; i < size; ++i)
        if (path[i] != 'X') return px_result(STATUS_BAD_ARGS);
    const auto now = sys::clock_now();
    if (now.status != STATUS_OK) return px_result(now.status);
    constexpr char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    constexpr uint64_t names = 36ull * 36 * 36 * 36 * 36 * 36;
    const auto start = now.value % names;
    auto candidate = start;
    do {
        auto value = candidate;
        for (size_t i = size; i != size - 6;) {
            path[--i] = digits[value % 36];
            value /= 36;
        }
        const int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0);
        if (fd >= 0 || errno != EEXIST) return fd;
        candidate = (candidate + 1) % names;
    } while (candidate != start);
    return px_result(STATUS_BUSY);
}

extern "C" int dup(int fd) {
    auto* value = desc(fd);
    if (!value) { errno = EBADF; return -1; }
    size_t next{};
    while (next < fds.size() && fds[next]) ++next;
    if (next == fds.size()) fds.push_back(nullptr);
    fds[next] = value;
    ++value->refs;
    return next;
}

extern "C" ssize_t read(int fd, void* data, size_t size) {
    auto* value = desc(fd);
    if (!value || size > INT64_MAX || (size && !data)) return px_result(STATUS_BAD_ARGS);
    if (auto* input = std::get_if<input_stream>(&value->data)) {
        if (size == 0) return px_result(0);
        if (!input->reader) {
            const auto status = input->reader.open(input->channel.selector(), events);
            if (status != STATUS_OK) return px_result(status);
        }
        if (input->offset == input->frame.size) {
            const auto status = input->reader.read(input->frame);
            if (status != STATUS_OK) return px_result(status);
            input->offset = 0;
        }
        const auto count = std::min(size, input->frame.size - input->offset);
        ::memcpy(data, input->frame.data + input->offset, count);
        input->offset += count;
        return px_result(count);
    }
    return px_result(file(fd) ? transfer(*value, io_operation::Read, data, size, 0, false) : STATUS_INVALID_CAP);
}

extern "C" ssize_t pread(int fd, void* data, size_t size, off_t offset) {
    if (offset < 0) { errno = EINVAL; return -1; }
    auto* opened = file(fd);
    return px_result(opened ? transfer(*opened, io_operation::Read, data, size, offset, true)
        : STATUS_INVALID_CAP);
}

extern "C" ssize_t pwrite(int fd, const void* data, size_t size, off_t offset) {
    if (offset < 0) { errno = EINVAL; return -1; }
    auto* opened = file(fd);
    return px_result(opened ? transfer(*opened, io_operation::Write,
        const_cast<void*>(data), size, offset, true) : STATUS_INVALID_CAP);
}

extern "C" int32_t px_submit(const io_request* request, request_id* id) {
    if (!request || !id || (request->operation != IO_READ && request->operation != IO_WRITE))
        return STATUS_BAD_ARGS;
    auto* opened = file(request->fd);
    if (!opened) return STATUS_BAD_ARGS;
    pending* item{};
    const auto status = enqueue(*opened, static_cast<io_operation>(request->operation),
        request->data, request->length, request->offset, false, false, item);
    if (status == STATUS_OK) *id = item->id;
    return status;
}

extern "C" int32_t px_completion(io_completion* result, uint32_t flags) {
    if (!result || (flags & ~IO_WAIT)) return STATUS_BAD_ARGS;
    for (;;) {
        pump();
        bool active{};
        for (auto& item : requests) if (item.file && !item.internal) {
            active = true;
            if (item.ready) {
                result->id = item.id;
                result->result = take(item);
                return 1;
            }
        }
        if (!active || !(flags & IO_WAIT)) return 0;
        const auto status = wait();
        if (status != STATUS_OK) return status;
    }
}

extern "C" int32_t px_cancel(request_id id) {
    pump();
    for (auto& item : requests) if (item.file && item.id == id && !item.internal) {
        if (item.ready) return STATUS_BUSY;
        ch_msg request{
            .operation = static_cast<uint64_t>(io_control::Cancel), .value = item.wire};
        const auto status = item.file->queue().exchange(request);
        return status == STATUS_NOT_FOUND ? STATUS_BUSY : status;
    }
    return STATUS_NOT_FOUND;
}

extern "C" int32_t fsync(int32_t fd) {
    auto* opened = file(fd);
    if (!opened) return px_result(STATUS_BAD_ARGS);
    if (opened->queue().requests().error()) return px_result(opened->queue().requests().error());
    const auto drained = drain(opened);
    if (drained != STATUS_OK) return px_result(drained);
    if (opened->queue().requests().error()) return px_result(opened->queue().requests().error());
    ch_msg request{
        .operation = static_cast<uint64_t>(io_control::Sync)};
    return px_result(opened->queue().exchange(request));
}

extern "C" int32_t px_sync(void) {
    const auto status = drain();
    if (status != STATUS_OK) return status;
    const auto connected = connect_vfs();
    return connected == STATUS_OK ? st_sync(vfs) : connected;
}

extern "C" int32_t px_size(int32_t fd, uint64_t* size) {
    auto* opened = file(fd);
    if (!opened || !size) return STATUS_BAD_ARGS;
    if (opened->queue().requests().error()) return opened->queue().requests().error();
    ch_msg request{.operation = static_cast<uint64_t>(io_control::Stat)};
    const auto status = opened->queue().exchange(request);
    if (status == STATUS_OK) *size = request.value;
    return status;
}

extern "C" int32_t px_mkdir(const char* path) {
    if (!path) return STATUS_BAD_ARGS;
    const auto status = connect_vfs();
    return status == STATUS_OK ? st_mkdir(vfs, path) : status;
}

extern "C" int32_t unlink(const char* path) {
    if (!path) return px_result(STATUS_BAD_ARGS);
    const auto status = connect_vfs();
    return px_result(status == STATUS_OK ? st_remove(vfs, path) : status);
}

extern "C" int32_t rename(const char* from, const char* to) {
    if (!from || !to) return px_result(STATUS_BAD_ARGS);
    const auto status = connect_vfs();
    return px_result(status == STATUS_OK ? st_rename(vfs, from, to) : status);
}

extern "C" ssize_t write(int fd, const void* data, size_t size) {
    if (auto* opened = file(fd))
        return px_result(transfer(*opened, io_operation::Write,
            const_cast<void*>(data), size, 0, false));
    auto* value = desc(fd);
    auto* output = value ? std::get_if<output_stream>(&value->data) : nullptr;
    if (!output) return px_result(STATUS_BAD_ARGS);
    if ((size != 0 && data == nullptr) || size > INT64_MAX) return px_result(STATUS_BAD_ARGS);

    const auto* bytes = static_cast<const char*>(data);
    uint64_t done{};
    while (done < size) {
        ch_msg message{};
        const auto chunk = size - done < sizeof(message.data)
            ? size - done : sizeof(message.data);
        message.size = chunk;
        ::memcpy(message.data, bytes + done, chunk);
        const auto status = ch_send(output->channel.selector(), message).status;
        if (status != STATUS_OK) return px_result(done != 0 ? static_cast<int64_t>(done) : status);
        done += chunk;
    }
    return px_result(done);
}
