#include <string.h>
#include <utility>
#include <variant>
#include <array>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/io.h>
#include <sys/storage.h>
#include <time.h>
#include <libk/checked_arithmetic.hpp>

#include <start.hpp>
#include <ch.hpp>
#include <proto.hpp>

struct directory {
    ch_msg batch{};
    char path[sizeof(ch_msg::data) + 1]{};
    uint64_t pos{};
    bool live{}, end{};
};


constexpr int Fds = 11;
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
static std::array<open_desc, Fds> opened;
static std::array<open_desc*, Fds> fds{};
static ch_rpc vfs;
static bool connected{};
static cap_t vspace{}, events{};
static cap_t vfs_cap{};
static cap_t cspace{}, store_admin{};
static cap_t pool{};
static cap_owner timer;
static directory dirs[4];
static auto desc(int fd) noexcept -> open_desc* {
    return fd >= 0 && fd < Fds ? fds[fd] : nullptr;
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


extern "C" int main(int argc, char** argv);

extern "C" DIR* opendir(const char* path) {
    if (!path || ::strlen(path) > sizeof(ch_msg::data) || connect_vfs() != STATUS_OK)
        return nullptr;
    for (auto& dir : dirs) if (!dir.live) {
        dir = {};
        ::memcpy(dir.path, path, ::strlen(path) + 1);
        if (st_list(vfs, dir.batch, dir.path) != STATUS_OK) return nullptr;
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
        const auto status = st_list(vfs, dir->batch, dir->path);
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

extern "C" void rt_init_io() noexcept {
    const auto& info = rt_startup();
    for (int fd = 0; fd < 3; ++fd) {
        const auto imported = info.selector(fd == 0 ? rt_stdin : fd == 1 ? rt_stdout : rt_stderr);
        if (!imported) continue;
        auto& value = opened[fd];
        if (fd == 0) value.data.emplace<input_stream>().channel = cap_owner{imported};
        else value.data.emplace<output_stream>().channel = cap_owner{imported};
        value.refs = 1;
        fds[fd] = &value;
    }
    vspace = info.vspace;
    events = info.events;
    vfs_cap = info.selector(vfs_service);
    if (vfs_cap == 0) vfs_cap = info.selector(vfs_read);
    cspace = info.cspace;
    store_admin = info.selector(st_admin);
    pool = info.pool;

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
        timer = cap_owner{created.value};
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
    const uint32_t mode = flags & 3u;
    if (mode == O_RDONLY && (flags & (O_CREAT | O_TRUNC | O_EXCL | O_APPEND)))
        return STATUS_BAD_ARGS;
    int fd = 0, slot = 0;
    while (fd < Fds && fds[fd]) ++fd;
    while (slot < Fds && !std::holds_alternative<std::monostate>(opened[slot].data)) ++slot;
    if (fd == Fds || slot == Fds) return STATUS_NO_MEMORY;
    const auto connected_status = connect_vfs();
    if (connected_status != STATUS_OK) return connected_status;
    uint64_t backend = mode == O_RDONLY ? st_read
        : mode == O_WRONLY ? st_write
        : st_read | st_write;
    if (flags & O_CREAT) backend |= st_create;
    if (flags & O_TRUNC) backend |= st_truncate;
    if (flags & O_EXCL) backend |= st_exclusive;
    if (flags & O_APPEND) backend |= st_append;
    cap_owner object;
    uint64_t length{}, expected{};
    auto status = st_open(vfs, path, backend, object, length, expected);
    if (status != STATUS_OK) return status;
    uint64_t generation{};
    auto& session = opened[slot].data.emplace<io_client>();
    status = session.adopt(std::move(object), events, vspace,
        0, generation, mode != O_RDONLY);
    if (status != STATUS_OK) { opened[slot].data.emplace<std::monostate>(); return status; }
    if (generation != expected) {
        (void)session.close();
        opened[slot].data.emplace<std::monostate>();
        return STATUS_PEER_FAULT;
    }
    opened[slot].refs = 1;
    fds[fd] = &opened[slot];
    return fd;
}

extern "C" int32_t close(int32_t fd) {
    auto* value = desc(fd);
    if (!value) return STATUS_BAD_ARGS;
    const auto status = release(*value);
    if (status == STATUS_OK || std::holds_alternative<std::monostate>(value->data)) fds[fd] = nullptr;
    return status;
}

extern "C" int32_t mkstemp(char* path) {
    if (!path) return STATUS_BAD_ARGS;
    const auto size = ::strlen(path);
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
    auto* value = desc(fd);
    if (!value) return STATUS_BAD_ARGS;
    for (int next = 0; next < Fds; ++next) if (!fds[next]) {
        fds[next] = value;
        ++value->refs;
        return next;
    }
    return STATUS_NO_MEMORY;
}

extern "C" int64_t read(int32_t fd, void* data, uint64_t size) {
    auto* value = desc(fd);
    if (!value || size > INT64_MAX || (size && !data)) return STATUS_BAD_ARGS;
    if (auto* input = std::get_if<input_stream>(&value->data)) {
        if (size == 0) return 0;
        if (!input->reader) {
            const auto status = input->reader.open(input->channel.selector(), events);
            if (status != STATUS_OK) return status;
        }
        if (input->offset == input->frame.size) {
            const auto status = input->reader.read(input->frame);
            if (status != STATUS_OK) return status;
            input->offset = 0;
        }
        const auto count = std::min(size, input->frame.size - input->offset);
        ::memcpy(data, input->frame.data + input->offset, count);
        input->offset += count;
        return count;
    }
    return file(fd) ? transfer(*value, io_operation::Read, data, size, 0, false) : STATUS_BAD_ARGS;
}

extern "C" int64_t pread(int32_t fd, void* data, uint64_t size, uint64_t offset) {
    auto* opened = file(fd);
    return opened ? transfer(*opened, io_operation::Read, data, size, offset, true)
        : STATUS_BAD_ARGS;
}

extern "C" int64_t pwrite(int32_t fd, const void* data, uint64_t size, uint64_t offset) {
    auto* opened = file(fd);
    return opened ? transfer(*opened, io_operation::Write,
        const_cast<void*>(data), size, offset, true) : STATUS_BAD_ARGS;
}

extern "C" int32_t submit(const io_request* request, request_id* id) {
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
        ch_msg request{
            .operation = static_cast<uint64_t>(io_control::Cancel), .value = item.wire};
        const auto status = item.file->queue().exchange(request);
        return status == STATUS_NOT_FOUND ? STATUS_BUSY : status;
    }
    return STATUS_NOT_FOUND;
}

extern "C" int32_t fsync(int32_t fd) {
    auto* opened = file(fd);
    if (!opened) return STATUS_BAD_ARGS;
    if (opened->queue().requests().error()) return opened->queue().requests().error();
    const auto drained = drain(opened);
    if (drained != STATUS_OK) return drained;
    if (opened->queue().requests().error()) return opened->queue().requests().error();
    ch_msg request{
        .operation = static_cast<uint64_t>(io_control::Sync)};
    return opened->queue().exchange(request);
}

extern "C" int32_t sync(void) {
    const auto status = drain();
    if (status != STATUS_OK) return status;
    const auto connected = connect_vfs();
    return connected == STATUS_OK ? st_sync(vfs) : connected;
}

extern "C" int32_t fstat(int32_t fd, struct stat* info) {
    auto* opened = file(fd);
    if (!opened || !info) return STATUS_BAD_ARGS;
    if (opened->queue().requests().error()) return opened->queue().requests().error();
    ch_msg request{.operation = static_cast<uint64_t>(io_control::Stat)};
    const auto status = opened->queue().exchange(request);
    if (status == STATUS_OK) info->size = request.value;
    return status;
}

extern "C" int32_t mkdir(const char* path) {
    if (!path) return STATUS_BAD_ARGS;
    const auto status = connect_vfs();
    return status == STATUS_OK ? st_mkdir(vfs, path) : status;
}

extern "C" int32_t device_id(uint8_t id[20]) {
    if (!id) return STATUS_BAD_ARGS;
    const auto status = connect_vfs();
    if (status != STATUS_OK) return status;
    uint8_t bytes[20]{};
    const auto result = st_device_id(vfs, bytes);
    if (result == STATUS_OK) ::memcpy(id, bytes, sizeof(bytes));
    return result;
}

extern "C" int32_t volume_id(uint8_t id[16]) {
    if (!id) return STATUS_BAD_ARGS;
    const auto status = connect_vfs();
    if (status != STATUS_OK) return status;
    uint8_t bytes[16]{};
    const auto result = st_volume_id(vfs, bytes);
    if (result == STATUS_OK) ::memcpy(id, bytes, sizeof(bytes));
    return result;
}

extern "C" int32_t format_volume(const uint8_t* id, uint32_t size) {
    if ((size != 0 && size != 16) || (size && !id)) return STATUS_BAD_ARGS;
    if (!store_admin) return STATUS_DENIED;
    ch_rpc admin;
    const auto status = admin.open(store_admin, pool, cspace, events);
    return status == STATUS_OK ? st_format(admin, size ? id : nullptr) : status;
}

extern "C" int32_t unlink(const char* path) {
    if (!path) return STATUS_BAD_ARGS;
    const auto status = connect_vfs();
    return status == STATUS_OK ? st_remove(vfs, path) : status;
}

extern "C" int32_t rename(const char* from, const char* to) {
    if (!from || !to) return STATUS_BAD_ARGS;
    const auto status = connect_vfs();
    return status == STATUS_OK ? st_rename(vfs, from, to) : status;
}

extern "C" int64_t write(int32_t fd, const void* data, uint64_t size) {
    if (auto* opened = file(fd))
        return transfer(*opened, io_operation::Write,
            const_cast<void*>(data), size, 0, false);
    auto* value = desc(fd);
    auto* output = value ? std::get_if<output_stream>(&value->data) : nullptr;
    if (!output) return STATUS_BAD_ARGS;
    if ((size != 0 && data == nullptr) || size > INT64_MAX) return STATUS_BAD_ARGS;

    const auto* bytes = static_cast<const char*>(data);
    uint64_t done{};
    while (done < size) {
        ch_msg message{};
        const auto chunk = size - done < sizeof(message.data)
            ? size - done : sizeof(message.data);
        message.size = chunk;
        ::memcpy(message.data, bytes + done, chunk);
        const auto status = ch_send(output->channel.selector(), message).status;
        if (status != STATUS_OK) return done != 0 ? done : status;
        done += chunk;
    }
    return done;
}
