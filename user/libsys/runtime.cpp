#include <utility>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/io.h>
#include <sys/storage.h>
#include <time.h>
#include <libk/checked_arithmetic.hpp>

#include <sys/start.hpp>
#include <sys/channel.hpp>
#include <sys/storage.hpp>

struct directory {
    sys::io::ControlMessage batch{};
    char path[81]{};
    uint64_t pos{};
    bool live{}, end{};
};

namespace {
cap_t streams[3]{};
sys::stream::Reader input;
sys::service::Message input_frame{};
uint64_t input_offset{};
bool input_open{}, input_end{};
constexpr int Fds = 11, Objects = 8;
// VFS and storage-admin sessions precede the per-open sessions.
constexpr uintptr_t Sessions = 0x76000000, Stride = 0x100000;
struct open_file {
    sys::io::ClientSession session;
    int refs{};
};
open_file files[Objects];
int fds[Fds]{};
sys::store::client vfs;
bool connected{};
cap_t vspace{}, events{};
cap_t vfs_cap{};
const void* boot_address{};
word_t boot_size{};
cap_t pool{};
sys::cap::OwnedCap timer;
directory dirs[4];
auto file(int fd) noexcept -> open_file* {
    if (fd < 3 || fd >= Fds || fds[fd] == 0) return nullptr;
    return &files[fds[fd] - 1];
}
auto connect_vfs() noexcept -> status_t {
    if (connected) return STATUS_OK;
    if (!vfs_cap) return STATUS_INVALID_CAP;
    const auto info = boot::BootView::parse(boot_address, boot_size);
    if (!info) return STATUS_BAD_ARGS;
    const auto import = info->selector(boot::Vfs)
        ? boot::Vfs : boot::VfsRead;
    const auto status = vfs.connect(*info, import, Sessions);
    if (status == STATUS_OK) connected = true;
    return status;
}
struct pending {
    open_file* file{};
    request_id id{}, wire{};
    int64_t result{};
    bool ready{}, internal{};
};
static_assert(IO_DEPTH == sys::io::QueueDepth && IO_LIMIT == sys::io::BufferSize);
pending requests[IO_DEPTH];
request_id next_id{};
auto release(open_file& file) noexcept -> int32_t {
    return --file.refs ? STATUS_OK : file.session.close();
}
auto enqueue(open_file& file, sys::io::Operation op, void* data, uint64_t size,
    uint64_t offset, bool implicit, bool internal, pending*& out) noexcept -> int32_t {
    if (next_id == UINT64_MAX || (size && !data)) return STATUS_BAD_ARGS;
    for (auto& item : requests) if (!item.file) {
        sys::io::Request request{.operation = static_cast<uint64_t>(op),
            .offset = offset, .length = size, .flags = implicit ? 1u : 0u};
        const auto submitted = file.session.submit(request, data);
        if (submitted != STATUS_OK) return submitted;
        item = {.file = &file, .id = ++next_id, .wire = request.id, .internal = internal};
        ++file.refs;
        out = &item;
        return STATUS_OK;
    }
    return STATUS_BUSY;
}
void pump() noexcept {
    for (auto& file : files) if (file.refs) {
        sys::io::Completion result{};
        while (file.session.completion(result) == STATUS_OK) {
            for (auto& item : requests) if (item.file == &file && item.wire == result.id) {
                item.result = result.status == STATUS_OK
                    ? static_cast<int64_t>(result.bytes) : result.status;
                item.ready = true;
                break;
            }
        }
    }
}
auto wait() noexcept -> int32_t {
    for (auto& file : files) if (file.refs && !file.session.requests().error()) {
        const auto status = file.session.arm();
        if (status != STATUS_OK) {
            (void)file.session.requests().fail(status);
            return STATUS_OK;
        }
    }
    const auto status = sys::notification_wait(events).status;
    if (status != STATUS_OK)
        for (auto& file : files) if (file.refs) (void)file.session.requests().fail(status);
    return status;
}
auto take(pending& item) noexcept -> int64_t {
    const auto result = item.result;
    auto* file = item.file;
    item = {};
    (void)release(*file);
    return result;
}
auto drain(open_file* target = nullptr) noexcept -> int32_t {
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
auto finish(pending& item) noexcept -> int64_t {
    while (!item.ready) {
        pump();
        if (item.ready) break;
        const auto status = wait();
        if (status != STATUS_OK) (void)item.file->session.requests().fail(status);
    }
    return take(item);
}
auto transfer(open_file& file, sys::io::Operation op, void* data, uint64_t size,
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
}

extern "C" int main(int argc, char** argv);

extern "C" DIR* opendir(const char* path) {
    if (!path || sys::service::length(path) > 80 || connect_vfs() != STATUS_OK)
        return nullptr;
    for (auto& dir : dirs) if (!dir.live) {
        dir = {};
        sys::service::copy(dir.path, path, sys::service::length(path) + 1);
        if (vfs.list(dir.batch, dir.path) != STATUS_OK) return nullptr;
        dir.live = true;
        return &dir;
    }
    return nullptr;
}

extern "C" int32_t readdir(DIR* dir, dirent* entry) {
    bool valid{};
    for (auto& candidate : dirs) if (dir == &candidate && candidate.live) valid = true;
    if (!valid || !entry) return STATUS_BAD_ARGS;
    while (dir->pos == dir->batch.size) {
        if (dir->end || dir->batch.value == 0) { dir->end = true; return 0; }
        dir->batch = {.value = dir->batch.value};
        const auto status = vfs.list(dir->batch, dir->path);
        if (status != STATUS_OK) return status;
        dir->pos = 0;
    }
    uint64_t size{};
    while (dir->pos < dir->batch.size && dir->batch.data[dir->pos])
        entry->name[size++] = dir->batch.data[dir->pos++];
    entry->name[size] = '\0';
    if (dir->pos < dir->batch.size) ++dir->pos;
    return 1;
}

extern "C" int32_t closedir(DIR* dir) {
    for (auto& candidate : dirs) if (dir == &candidate && candidate.live) {
        candidate = {};
        return STATUS_OK;
    }
    return STATUS_BAD_ARGS;
}

extern "C" [[noreturn]] void program_start(
    const void* address, word_t size, const char* arg_data, size_t arg_size) noexcept {
    const auto info = boot::BootView::parse(address, size);
    if (!info) sys::exit(STATUS_BAD_ARGS);
    streams[STDIN_FILENO] = info->selector(boot::Stdin);
    streams[STDOUT_FILENO] = info->selector(boot::Stdout);
    streams[STDERR_FILENO] = info->selector(boot::Stderr);
    vspace = info->selector(BOOT_VSPACE);
    events = info->selector(BOOT_EVENTS);
    vfs_cap = info->selector(boot::Vfs);
    if (vfs_cap == 0) vfs_cap = info->selector(boot::VfsRead);
    boot_address = address;
    boot_size = size;
    pool = info->selector(BOOT_POOL);

    boot::Args args;
    if (!args.decode(arg_data, arg_size)) sys::exit(STATUS_BAD_ARGS);
    char* argv[boot::Args::Max + 1]{};
    const auto argc = static_cast<int>(args.count());
    for (int i = 0; i < argc; ++i) argv[i] = const_cast<char*>(args.argument(i));
    sys::exit(main(argc, argv));
}

extern "C" int32_t clock_gettime(int32_t clock, timespec* time) {
    if (clock != CLOCK_MONOTONIC || !time) return STATUS_BAD_ARGS;
    const auto frequency = sys::clock_frequency();
    if (frequency.status != STATUS_OK) return frequency.status;
    const auto now = sys::clock_now();
    if (now.status != STATUS_OK) return now.status;
    const auto fraction = libk::checked_multiply(now.value % frequency.value, uint64_t{1'000'000'000});
    if (!fraction || now.value / frequency.value > INT64_MAX) return STATUS_BAD_ARGS;
    *time = {static_cast<int64_t>(now.value / frequency.value),
        static_cast<int64_t>(*fraction / frequency.value)};
    return STATUS_OK;
}

extern "C" int32_t nanosleep(const timespec* delay, timespec* remaining) {
    if (!delay || delay->tv_sec < 0 || delay->tv_nsec < 0
        || delay->tv_nsec >= 1'000'000'000) return STATUS_BAD_ARGS;
    const auto frequency = sys::clock_frequency();
    if (frequency.status != STATUS_OK) return frequency.status;
    const auto seconds = libk::checked_multiply(static_cast<uint64_t>(delay->tv_sec), frequency.value);
    const auto fraction = libk::checked_multiply(static_cast<uint64_t>(delay->tv_nsec), frequency.value);
    if (!seconds || !fraction) return STATUS_BAD_ARGS;
    const auto ticks = libk::checked_add(*seconds,
        *fraction / 1'000'000'000 + (*fraction % 1'000'000'000 != 0));
    if (!ticks) return STATUS_BAD_ARGS;
    const auto now = sys::clock_now();
    if (now.status != STATUS_OK) return now.status;
    const auto deadline = libk::checked_add(now.value, *ticks);
    if (!deadline) return STATUS_BAD_ARGS;
    if (*ticks == 0) { if (remaining) *remaining = {}; return STATUS_OK; }
    if (!timer) {
        const auto created = sys::notification_create(pool, 1);
        if (created.status != STATUS_OK) return created.status;
        timer = sys::cap::OwnedCap{{created.value, 0}};
    }
    const auto status = sys::notification_wait(timer.selector(), *deadline).status;
    if (status == STATUS_TIMED_OUT) {
        if (remaining) *remaining = {};
        return STATUS_OK;
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
    return status;
}

extern "C" int32_t open(const char* path, uint32_t flags) {
    if (!path || !vfs_cap || (flags & ~(3u | O_CREAT | O_TRUNC | O_EXCL | O_APPEND)) != 0
        || (flags & 3u) == 3u) return STATUS_BAD_ARGS;
    const uint32_t access = flags & 3u;
    if (access == O_RDONLY && (flags & (O_CREAT | O_TRUNC | O_EXCL | O_APPEND)))
        return STATUS_BAD_ARGS;
    int fd = 3, slot = 0;
    while (fd < Fds && fds[fd]) ++fd;
    while (slot < Objects && files[slot].refs) ++slot;
    if (fd == Fds || slot == Objects) return STATUS_NO_MEMORY;
    const auto connected_status = connect_vfs();
    if (connected_status != STATUS_OK) return connected_status;
    uint64_t backend = access == O_RDONLY ? sys::store::Read
        : access == O_WRONLY ? sys::store::Write
        : sys::store::Read | sys::store::Write;
    if (flags & O_CREAT) backend |= sys::store::Create;
    if (flags & O_TRUNC) backend |= sys::store::Truncate;
    if (flags & O_EXCL) backend |= sys::store::Exclusive;
    if (flags & O_APPEND) backend |= sys::store::Append;
    sys::cap::OwnedCap object;
    uint64_t length{}, expected{};
    auto status = vfs.open(path, backend, object, length, expected);
    if (status != STATUS_OK) return status;
    uint64_t generation{};
    status = files[slot].session.adopt(std::move(object), events, vspace,
        Sessions + (slot + 2) * Stride, generation, access != O_RDONLY);
    if (status != STATUS_OK) return status;
    if (generation != expected) {
        (void)files[slot].session.close();
        return STATUS_PEER_FAULT;
    }
    files[slot].refs = 1;
    fds[fd] = slot + 1;
    return fd;
}

extern "C" int32_t close(int32_t fd) {
    auto* opened = file(fd);
    if (!opened) return STATUS_BAD_ARGS;
    fds[fd] = 0;
    return release(*opened);
}

extern "C" int32_t mkstemp(char* path) {
    if (!path) return STATUS_BAD_ARGS;
    const auto size = sys::service::length(path);
    if (size < 6) return STATUS_BAD_ARGS;
    for (size_t i = size - 6; i < size; ++i)
        if (path[i] != 'X') return STATUS_BAD_ARGS;
    const auto now = sys::clock_now();
    if (now.status != STATUS_OK) return now.status;
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
        const int fd = open(path, O_RDWR | O_CREAT | O_EXCL);
        if (fd != STATUS_BUSY) return fd;
        candidate = (candidate + 1) % names;
    } while (candidate != start);
    return STATUS_BUSY;
}

extern "C" int32_t dup(int32_t fd) {
    auto* opened = file(fd);
    if (!opened) return STATUS_BAD_ARGS;
    for (int next = 3; next < Fds; ++next) if (!fds[next]) {
        fds[next] = fds[fd];
        ++opened->refs;
        return next;
    }
    return STATUS_NO_MEMORY;
}

extern "C" int64_t read(int32_t fd, void* data, uint64_t size) {
    if (fd == STDIN_FILENO) {
        if (size > INT64_MAX || (size && !data)) return STATUS_BAD_ARGS;
        if (!input_open) {
            const auto status = input.open(streams[STDIN_FILENO], events);
            if (status != STATUS_OK) return status;
            input_open = true;
        }
        if (size == 0 || input_end) return 0;
        if (input_offset == input_frame.size) {
            const auto status = input.read(input_frame);
            if (status != STATUS_OK) return status;
            if (input_frame.size == 0) { input_end = true; return 0; }
            input_offset = 0;
        }
        const auto available = input_frame.size - input_offset;
        const auto count = size < available ? size : available;
        sys::service::copy(data, input_frame.data + input_offset, count);
        input_offset += count;
        return count;
    }
    auto* opened = file(fd);
    return opened ? transfer(*opened, sys::io::Operation::Read, data, size, 0, false)
        : STATUS_BAD_ARGS;
}

extern "C" int64_t pread(int32_t fd, void* data, uint64_t size, uint64_t offset) {
    auto* opened = file(fd);
    return opened ? transfer(*opened, sys::io::Operation::Read, data, size, offset, true)
        : STATUS_BAD_ARGS;
}

extern "C" int64_t pwrite(int32_t fd, const void* data, uint64_t size, uint64_t offset) {
    auto* opened = file(fd);
    return opened ? transfer(*opened, sys::io::Operation::Write,
        const_cast<void*>(data), size, offset, true) : STATUS_BAD_ARGS;
}

extern "C" int32_t submit(const io_request* request, request_id* id) {
    if (!request || !id || (request->operation != IO_READ && request->operation != IO_WRITE))
        return STATUS_BAD_ARGS;
    auto* opened = file(request->fd);
    if (!opened) return STATUS_BAD_ARGS;
    pending* item{};
    const auto status = enqueue(*opened, static_cast<sys::io::Operation>(request->operation),
        request->data, request->length, request->offset, false, false, item);
    if (status == STATUS_OK) *id = item->id;
    return status;
}

extern "C" int32_t completion(io_completion* result, uint32_t flags) {
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

extern "C" int32_t cancel(request_id id) {
    pump();
    for (auto& item : requests) if (item.file && item.id == id && !item.internal) {
        if (item.ready) return STATUS_BUSY;
        sys::io::ControlMessage request{
            .operation = static_cast<uint64_t>(sys::io::Control::Cancel), .value = item.wire};
        const auto status = item.file->session.exchange(request);
        return status == STATUS_NOT_FOUND ? STATUS_BUSY : status;
    }
    return STATUS_NOT_FOUND;
}

extern "C" int32_t fsync(int32_t fd) {
    auto* opened = file(fd);
    if (!opened) return STATUS_BAD_ARGS;
    if (opened->session.requests().error()) return opened->session.requests().error();
    const auto drained = drain(opened);
    if (drained != STATUS_OK) return drained;
    if (opened->session.requests().error()) return opened->session.requests().error();
    sys::io::ControlMessage request{
        .operation = static_cast<uint64_t>(sys::io::Control::Sync)};
    return opened->session.exchange(request);
}

extern "C" int32_t sync(void) {
    const auto status = drain();
    if (status != STATUS_OK) return status;
    const auto connected = connect_vfs();
    return connected == STATUS_OK ? vfs.sync() : connected;
}

extern "C" int32_t fstat(int32_t fd, struct stat* info) {
    auto* opened = file(fd);
    if (!opened || !info) return STATUS_BAD_ARGS;
    if (opened->session.requests().error()) return opened->session.requests().error();
    sys::io::ControlMessage request{.operation = static_cast<uint64_t>(sys::io::Control::Stat)};
    const auto status = opened->session.exchange(request);
    if (status == STATUS_OK) info->size = request.value;
    return status;
}

extern "C" int32_t mkdir(const char* path) {
    if (!path) return STATUS_BAD_ARGS;
    const auto status = connect_vfs();
    return status == STATUS_OK ? vfs.mkdir(path) : status;
}

extern "C" int32_t device_id(uint8_t id[20]) {
    if (!id) return STATUS_BAD_ARGS;
    const auto status = connect_vfs();
    if (status != STATUS_OK) return status;
    uint8_t bytes[20]{};
    const auto result = vfs.device_id(bytes);
    if (result == STATUS_OK) sys::service::copy(id, bytes, sizeof(bytes));
    return result;
}

extern "C" int32_t volume_id(uint8_t id[16]) {
    if (!id) return STATUS_BAD_ARGS;
    const auto status = connect_vfs();
    if (status != STATUS_OK) return status;
    uint8_t bytes[16]{};
    const auto result = vfs.volume_id(bytes);
    if (result == STATUS_OK) sys::service::copy(id, bytes, sizeof(bytes));
    return result;
}

extern "C" int32_t format_volume(const uint8_t* id, uint32_t size) {
    if ((size != 0 && size != 16) || (size && !id)) return STATUS_BAD_ARGS;
    const auto info = boot::BootView::parse(boot_address, boot_size);
    if (!info) return STATUS_BAD_ARGS;
    if (!info->selector(boot::StoreAdmin)) return STATUS_DENIED;
    sys::store::client admin;
    auto status = admin.connect(*info, boot::StoreAdmin, Sessions + Stride);
    if (status == STATUS_OK) status = admin.format(size ? id : nullptr);
    const auto closed = admin.close();
    return status == STATUS_OK ? closed : status;
}

extern "C" int32_t unlink(const char* path) {
    if (!path) return STATUS_BAD_ARGS;
    const auto status = connect_vfs();
    return status == STATUS_OK ? vfs.remove(path) : status;
}

extern "C" int32_t rename(const char* from, const char* to) {
    if (!from || !to) return STATUS_BAD_ARGS;
    const auto status = connect_vfs();
    return status == STATUS_OK ? vfs.rename(from, to) : status;
}

extern "C" int64_t write(int32_t fd, const void* data, uint64_t size) {
    if (auto* opened = file(fd))
        return transfer(*opened, sys::io::Operation::Write,
            const_cast<void*>(data), size, 0, false);
    if (fd != STDOUT_FILENO && fd != STDERR_FILENO) return STATUS_BAD_ARGS;
    if ((size != 0 && data == nullptr) || size > INT64_MAX) return STATUS_BAD_ARGS;
    const auto output = streams[fd];
    if (output == 0) return STATUS_INVALID_CAP;
    const auto* bytes = static_cast<const char*>(data);
    uint64_t done{};
    while (done < size) {
        sys::service::Message message{};
        const auto chunk = size - done < sizeof(message.data)
            ? size - done : sizeof(message.data);
        message.size = chunk;
        sys::service::copy(message.data, bytes + done, chunk);
        const auto status = sys::service::send(output, message).status;
        if (status != STATUS_OK) return done != 0 ? done : status;
        done += chunk;
    }
    return done;
}
