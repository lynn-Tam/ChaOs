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
#include <cstdlib>
#include <unistd.h>
#include <dirent.h>
#include <posix.hpp>
#include <time.h>
#include <libk/checked_arithmetic.hpp>

#include <ubus/ubus.hpp>
#include <vm.hpp>

static ubus_conn dir_rpc, dir_bus;
static constexpr size_t px_depth = 32, px_limit = 4096;
enum class px_op { Read = 1, Write, Flush, Identify };
struct px_cmd { uint64_t id{}; px_op operation{}; uint64_t offset{}, buffer_offset{}, length{}; bool sequential{}; uint64_t deadline{}; };
struct px_done { uint64_t id{}; status_t status{}; uint64_t bytes{}; };
enum : uint64_t { px_read = 1, px_write = 2, px_create = 4, px_truncate = 8, px_exclusive = 16, px_append = 32 };
enum : status_t { px_exists = -256, px_not_dir, px_is_dir, px_not_empty, px_name_long, px_too_large, px_no_space, px_cross_device };

// This fd owns its buffer. The peer gets a revocable view and each outstanding
// call retains its exact slice until the native Reply completes.
class px_file {
    struct operation { px_cmd command; void* destination{}; bool exact{}; };
    ubus_conn peer_;
    vm_area buffer_;
    cap_desc descriptor_;
    cap_owner exported_;
    ubus_batch<operation, px_depth> calls_;
    uint64_t next_{}, loan_{};
    status_t error_{};
    bool writable_{}, opened_{};
public:
    ~px_file() {
        if (opened_) (void)close();
        else if (loan_) { ubus_msg q, r; q.data.put("action", "close").put("loan", loan_); (void)dir_bus.invoke("Resource", std::move(q), r); }
    }
    auto open(cap_owner endpoint, bool writable = false, uint64_t* capacity = nullptr, uint64_t loan = 0) -> status_t {
        loan_ = loan;
        auto status = peer_.adopt(std::move(endpoint)); if (status != STATUS_OK) return status;
        ubus_msg info; status = peer_.invoke("Stat", {}, info); if (status != STATUS_OK) return status;
        next_ = info.data["sequence"].as<uint64_t>().value_or(0);
        if (capacity) { const auto size = info.data["size"].as<uint64_t>(); if (!size) return STATUS_PEER_FAULT; *capacity = *size; }
        const auto made = sys::memory_create(rt_startup().alloc(), px_depth * px_limit, VM_READ | VM_WRITE);
        if (made.status != STATUS_OK) return made.status;
        auto mapped = vm_area::map(rt_vm(), cap_owner{made.value}, 0, px_depth * px_limit, VM_READ | VM_WRITE);
        if (!mapped) return mapped.error();
        buffer_ = std::move(*mapped);
        const CapView view{.version=CAP_ATTENUATION_VERSION_CURRENT, .kind=ObjMem, .size=CAP_ATTENUATION_SIZE,
            .rights=RIGHT_MAP | RIGHT_DUPLICATE | RIGHT_REVOKE, .words={0, px_depth, VM_READ | VM_WRITE}};
        auto grant = descriptor_.derive(buffer_.memory.selector(), view); if (!grant) return grant.error();
        exported_ = std::move(*grant); writable_ = writable; opened_ = true; return STATUS_OK;
    }
    auto submit(px_cmd& command, void* data = nullptr, bool exact = false) -> status_t {
        if (const auto status = error(); status != STATUS_OK) return status;
        if (!opened_ || next_ == UINT64_MAX || command.length > px_limit || command.offset > UINT64_MAX - command.length) return STATUS_BAD_ARGS;
        if (command.operation == px_op::Write && (!writable_ || (command.length && !data))) return STATUS_DENIED;
        static constexpr std::array<std::string_view, 4> names{"Read", "Write", "Flush", "Identify"};
        const auto kind = size_t(command.operation); if (!kind || kind > names.size()) return STATUS_BAD_ARGS;
        command.id = ++next_;
        return calls_.submit(command.id, operation{command, data, exact}, peer_, names[kind - 1],
            [&](size_t slot, operation& local, ubus_msg& message) {
                command.buffer_offset = slot * px_limit; local.command.buffer_offset = command.buffer_offset;
                if (command.operation == px_op::Write && command.length)
                    ::memcpy(reinterpret_cast<void*>(buffer_.address + command.buffer_offset), data, command.length);
                message.deadline = command.deadline; message.data.put("id", command.id);
                if (command.operation != px_op::Flush) {
                    const auto cap = message.attach(exported_.selector(), RIGHT_MAP | RIGHT_DUPLICATE); if (!cap) return STATUS_NO_MEMORY;
                    message.data.put("buffer", ubus_buffer{*cap, command.buffer_offset, command.length});
                    if (!command.sequential) message.data.put("offset", command.offset);
                }
                return STATUS_OK;
            });
    }
    auto completion(px_done& result, uint64_t id = 0) -> status_t {
        (void)error(); operation local; ubus_msg response;
        const auto status = calls_.completion(local, response, id); if (status != STATUS_OK) return status;
        const auto count = response.data["bytes"].as<uint64_t>();
        auto outcome = response.status;
        if (outcome == STATUS_OK && (!count || *count > local.command.length || (local.exact && *count != local.command.length))) outcome = STATUS_PEER_FAULT;
        const auto bytes = outcome == STATUS_OK ? *count : 0;
        if (bytes && local.destination && local.command.operation != px_op::Write)
            ::memcpy(local.destination, reinterpret_cast<const void*>(buffer_.address + local.command.buffer_offset), bytes);
        result = {local.command.id, outcome, bytes}; return STATUS_OK;
    }
    auto contains(uint64_t id) const -> bool { return calls_.contains(id); }
    auto next(uint64_t after = 0) const -> const px_cmd* { const auto* item = calls_.next(after); return item ? &item->command : nullptr; }
    auto cancel(uint64_t id) -> status_t { ubus_msg q, r; q.data.put("id", id); return peer_.invoke("Cancel", std::move(q), r); }
    auto invoke(std::string_view name, ubus_msg q, ubus_msg& r) -> status_t { return peer_.invoke(name, std::move(q), r); }
    auto fail(status_t status) -> status_t {
        if (!error_) {
            error_ = status;
            if (exported_) { const auto revoked = sys::cap_revoke(exported_.selector()).status; if (revoked != STATUS_OK) return revoked; exported_ = {}; }
            calls_.abort(status);
        }
        return status;
    }
    auto error() -> status_t { if (!error_ && opened_) { const auto status = peer_.state(); if (status != STATUS_OK) return fail(status); } return error_; }
    auto close() -> status_t {
        ubus_msg reply; const auto result = peer_.invoke("Close", {}, reply);
        (void)fail(result == STATUS_OK ? STATUS_CLOSED : result);
        const auto status = buffer_.close(); if (status != STATUS_OK) return status;
        opened_ = false; const auto closed = peer_.close();
        if (loan_) {
            ubus_msg request, response; request.data.put("action", "close").put("loan", std::exchange(loan_, 0));
            const auto returned = dir_bus.invoke("Resource", std::move(request), response);
            if (returned != STATUS_OK && result == STATUS_OK) return returned;
        }
        return result == STATUS_OK ? closed : result;
    }
};

static auto px_path(std::string_view path, std::string_view base = "/") -> std::string {
    std::string result = path.starts_with('/') ? std::string{path} : std::string{base} + "/" + std::string{path};
    size_t size{}, cursor{};
    while (cursor < result.size()) {
        const auto begin = cursor; while (cursor < result.size() && result[cursor] != '/') ++cursor;
        const std::string_view part{result.data() + begin, cursor - begin};
        if (part == "..") { while (size && result[size - 1] != '/') --size; if (size) --size; }
        else if (!part.empty() && part != ".") { if (size) result[size++] = '/'; ::memmove(result.data() + size, part.data(), part.size()); size += part.size(); }
        ++cursor;
    }
    result.resize(size); result.insert(0, "/"); return result;
}

// This adapter currently supports one user thread; errno is not TLS yet.
extern "C" auto __errno() -> int* {
    static int value;
    return &value;
}
static auto px_result(int64_t value) noexcept -> int64_t {
    if (value >= 0)
        return value;
    switch (value) {
    case STATUS_INVALID_CAP:
        errno = EBADF;
        break;
    case STATUS_INVALID_OP:
        errno = ENOTSUP;
        break;
    case STATUS_BAD_RIGHTS:
    case STATUS_DENIED:
        errno = EACCES;
        break;
    case STATUS_BAD_ARGS:
        errno = EINVAL;
        break;
    case STATUS_NOT_FOUND:
        errno = ENOENT;
        break;
    case STATUS_BUSY:
        errno = EBUSY;
        break;
    case STATUS_NO_MEMORY:
        errno = ENOMEM;
        break;
    case STATUS_WOULD_BLOCK:
        errno = EAGAIN;
        break;
    case STATUS_CANCELED:
        errno = ECANCELED;
        break;
    case STATUS_TIMED_OUT:
        errno = ETIMEDOUT;
        break;
    case STATUS_CLOSED:
    case STATUS_PEER_CLOSED:
        errno = EPIPE;
        break;
    case px_exists:
        errno = EEXIST;
        break;
    case px_not_dir:
        errno = ENOTDIR;
        break;
    case px_is_dir:
        errno = EISDIR;
        break;
    case px_not_empty:
        errno = ENOTEMPTY;
        break;
    case px_name_long:
        errno = ENAMETOOLONG;
        break;
    case px_too_large:
        errno = EFBIG;
        break;
    case px_cross_device:
        errno = EXDEV;
        break;
    case px_no_space:
        errno = ENOSPC;
        break;
    default:
        errno = EIO;
        break;
    }
    return -1;
}

static std::string cwd{"/"};

struct directory {
    ubus_data page;
    std::string path;
    size_t index{};
    dirent entry{};
};

struct input_stream {
    cap_owner channel;
    ubus_rx reader;
    ubus_msg frame;
    bool ended{};
    uint64_t offset{};
};
struct output_stream {
    cap_owner channel;
};
struct open_desc {
    std::variant<std::monostate, input_stream, output_stream, px_file> data;
    int refs{}; // fd references and pending completions share this owner.
    auto queue() noexcept -> px_file& { return std::get<px_file>(data); }
};
static std::deque<open_desc> opened{3};
static std::vector<open_desc*> fds(3);
static cap_t events{};
static auto files() -> status_t {
    if (dir_rpc.endpoint()) {
        const auto fact = sys::source_info(dir_rpc.endpoint(), SOURCE_WRITE);
        if (fact.status == STATUS_OK && !(fact.value & SOURCE_TERMINAL)) return STATUS_OK;
        (void)dir_rpc.close();
    }
    if (!dir_bus.endpoint()) {
        const auto status = dir_bus.bind(rt_startup().environment);
        if (status != STATUS_OK) return status;
    }
    ubus_msg request, response; request.data.put("name", "files");
    const auto status = dir_bus.invoke("Get", std::move(request), response);
    return status == STATUS_OK ? dir_rpc.bind(response.capability(response.data["connector"]), "io.files", 1, 1) : status;
}
static std::deque<std::unique_ptr<directory>> dirs;
static auto desc(int fd) noexcept -> open_desc* {
    return fd >= 0 && static_cast<size_t>(fd) < fds.size() ? fds[fd] : nullptr;
}
static auto file(int fd) noexcept -> open_desc* {
    auto* value = desc(fd);
    return value && std::holds_alternative<px_file>(value->data) ? value : nullptr;
}
struct pending {
    open_desc* file{};
    request_id id{}, wire{};
    int64_t result{};
    bool ready{}, internal{};
};
static_assert(IO_DEPTH == px_depth && IO_LIMIT == px_limit);
pending requests[IO_DEPTH];
static request_id next_id{};
static auto release(open_desc& value) noexcept -> int32_t {
    if (value.refs > 1) {
        --value.refs;
        return STATUS_OK;
    }
    status_t status{};
    if (auto* input = std::get_if<input_stream>(&value.data)) {
        status = input->reader.close();
        if (status != STATUS_OK)
            return status;
        status = input->channel.close();
        if (status != STATUS_OK)
            return status;
    } else if (auto* output = std::get_if<output_stream>(&value.data)) {
        status = output->channel.close();
        if (status != STATUS_OK)
            return status;
    } else
        status = value.queue().close();
    value.data.emplace<std::monostate>();
    value.refs = 0;
    return status;
}
static auto enqueue(open_desc& file, px_op op, void* data, uint64_t size, uint64_t offset,
                    bool implicit, bool internal, pending*& out) noexcept -> int32_t {
    if (next_id == UINT64_MAX || (size && !data))
        return STATUS_BAD_ARGS;
    for (auto& item : requests)
        if (!item.file) {
            px_cmd request{.operation = op,
                           .offset = offset,
                           .length = size,
                           .sequential = implicit};
            const auto submitted = file.queue().submit(request, data);
            if (submitted != STATUS_OK)
                return submitted;
            item = {.file = &file, .id = ++next_id, .wire = request.id, .internal = internal};
            ++file.refs;
            out = &item;
            return STATUS_OK;
        }
    return STATUS_BUSY;
}
static void pump() noexcept {
    for (auto& file : opened)
        if (std::holds_alternative<px_file>(file.data)) {
            px_done result{};
            while (file.queue().completion(result) == STATUS_OK) {
                for (auto& item : requests)
                    if (item.file == &file && item.wire == result.id) {
                        item.result = result.status == STATUS_OK
                                          ? static_cast<int64_t>(result.bytes)
                                          : result.status;
                        item.ready = true;
                        break;
                    }
            }
        }
}
static auto wait() noexcept -> int32_t {
    const auto status = ev_wait(events).status;
    if (status != STATUS_OK)
        for (auto& file : opened)
            if (std::holds_alternative<px_file>(file.data))
                (void) file.queue().fail(status);
    return status;
}
static auto take(pending& item) noexcept -> int64_t {
    const auto result = item.result;
    auto* file = item.file;
    item = {};
    (void) release(*file);
    return result;
}
static auto drain(open_desc* target = nullptr) noexcept -> int32_t {
    for (;;) {
        pump();
        bool active{};
        for (const auto& item : requests)
            if (item.file && (!target || item.file == target) && !item.ready)
                active = true;
        if (!active)
            break;
        const auto status = wait();
        if (status != STATUS_OK)
            return status;
    }
    return STATUS_OK;
}
static auto finish(pending& item) noexcept -> int64_t {
    while (!item.ready) {
        pump();
        if (item.ready)
            break;
        const auto status = wait();
        if (status != STATUS_OK)
            (void) item.file->queue().fail(status);
    }
    return take(item);
}
static auto transfer(open_desc& file, px_op op, void* data, uint64_t size, uint64_t offset,
                     bool explicit_offset) noexcept -> int64_t {
    if (size > INT64_MAX || (size && !data) || (explicit_offset && offset > UINT64_MAX - size))
        return STATUS_BAD_ARGS;
    uint64_t done{};
    while (done < size) {
        const auto count = size - done < IO_LIMIT ? size - done : uint64_t{IO_LIMIT};
        pending* item{};
        const auto status = enqueue(file, op, static_cast<uint8_t*>(data) + done, count,
                                    offset + done, !explicit_offset, true, item);
        if (status != STATUS_OK)
            return done ? static_cast<int64_t>(done) : status;
        const auto result = finish(*item);
        if (result < 0)
            return done ? static_cast<int64_t>(done) : result;
        done += result;
        if (static_cast<uint64_t>(result) < count)
            break;
    }
    return done;
}

extern "C" DIR* opendir(const char* path) {
    if (!path || !*path) { errno = EINVAL; return nullptr; }
    auto status = files();
    if (status != STATUS_OK) { (void)px_result(status); return nullptr; }
    auto dir = std::make_unique<directory>();
    dir->path = px_path(path, cwd);
    ubus_msg query, reply; query.data.put("path", dir->path);
    status = dir_rpc.invoke("List", std::move(query), reply); dir->page = std::move(reply.data);
    if (status != STATUS_OK) { (void)px_result(status); return nullptr; }
    auto* result = dir.get();
    for (auto& slot : dirs) if (!slot) { slot = std::move(dir); return result; }
    dirs.push_back(std::move(dir)); return result;
}

extern "C" dirent* readdir(DIR* dir) {
    if (std::ranges::none_of(dirs, [dir](const auto& slot) { return slot && slot.get() == dir; })) {
        errno = EBADF; return nullptr;
    }
    for (;;) {
        size_t index{};
        for (const auto item : dir->page["items"].values()) if (index++ == dir->index) {
            const auto name = item["name"].as<std::string_view>();
            if (!name || name->size() >= sizeof(dir->entry.d_name)) { errno = ENAMETOOLONG; return nullptr; }
            ::memcpy(dir->entry.d_name, name->data(), name->size());
            dir->entry.d_name[name->size()] = 0; ++dir->index; return &dir->entry;
        }
        const auto cursor = dir->page["cursor"];
        if (!cursor) return nullptr;
        ubus_data page;
        ubus_msg query, reply; query.data.put("path", dir->path).put("cursor", cursor);
        const auto status = dir_rpc.invoke("List", std::move(query), reply); page = std::move(reply.data);
        if (status != STATUS_OK) { (void)px_result(status); return nullptr; }
        dir->page = std::move(page); dir->index = 0;
    }
}

extern "C" int closedir(DIR* dir) {
    for (auto& slot : dirs)
        if (slot && slot.get() == dir) {
            slot.reset();
            return 0;
        }
    errno = EBADF;
    return -1;
}

auto px_init() noexcept -> status_t {
    if (events) return STATUS_BUSY;
    const auto& env = rt_startup();
    events = env.events;
    std::array<cap_owner, 3> streams;
    auto status = dir_bus.bind(env.environment); if (status != STATUS_OK) return status;
    ubus_msg setup; status = dir_bus.invoke("Receive", {}, setup); if (status != STATUS_OK) return status;
    const std::array<std::string_view, 3> fields{"stdin", "stdout", "stderr"};
    for (size_t fd = 0; fd < streams.size(); ++fd) streams[fd] = setup.take(setup.data["message"][fields[fd]]);
    for (auto** value = environ; value && *value; ++value)
        if (const std::string_view entry{*value}; entry.starts_with("PWD=/")) {
            cwd = px_path(entry.substr(4));
            break;
        }
    for (int fd = 0; fd < 3; ++fd) {
        if (!streams[fd])
            continue;
        auto& value = opened[fd];
        if (fd == 0)
            value.data.emplace<input_stream>().channel = std::move(streams[fd]);
        else
            value.data.emplace<output_stream>().channel = std::move(streams[fd]);
        value.refs = 1;
        fds[fd] = &value;
    }
    return STATUS_OK;
}

extern "C" int32_t clock_gettime(int32_t clock, timespec* time) {
    if (clock != CLOCK_MONOTONIC || !time)
        return px_result(STATUS_BAD_ARGS);
    const auto frequency = sys::clock_frequency();
    if (frequency.status != STATUS_OK)
        return px_result(frequency.status);
    const auto now = sys::clock_now();
    if (now.status != STATUS_OK)
        return px_result(now.status);
    const auto fraction =
        libk::checked_multiply(now.value % frequency.value, uint64_t{1'000'000'000});
    if (!fraction || now.value / frequency.value > INT64_MAX)
        return px_result(STATUS_BAD_ARGS);
    *time = {static_cast<int64_t>(now.value / frequency.value),
             static_cast<int64_t>(*fraction / frequency.value)};
    return px_result(STATUS_OK);
}

extern "C" int32_t nanosleep(const timespec* delay, timespec* remaining) {
    if (!delay || delay->tv_sec < 0 || delay->tv_nsec < 0 || delay->tv_nsec >= 1'000'000'000)
        return px_result(STATUS_BAD_ARGS);
    const auto frequency = sys::clock_frequency();
    if (frequency.status != STATUS_OK)
        return px_result(frequency.status);
    const auto seconds =
        libk::checked_multiply(static_cast<uint64_t>(delay->tv_sec), frequency.value);
    const auto fraction =
        libk::checked_multiply(static_cast<uint64_t>(delay->tv_nsec), frequency.value);
    if (!seconds || !fraction)
        return px_result(STATUS_BAD_ARGS);
    const auto ticks =
        libk::checked_add(*seconds, *fraction / 1'000'000'000 + (*fraction % 1'000'000'000 != 0));
    if (!ticks)
        return px_result(STATUS_BAD_ARGS);
    const auto now = sys::clock_now();
    if (now.status != STATUS_OK)
        return px_result(now.status);
    const auto deadline = libk::checked_add(now.value, *ticks);
    if (!deadline)
        return px_result(STATUS_BAD_ARGS);
    if (*ticks == 0) {
        if (remaining)
            *remaining = {};
        return px_result(STATUS_OK);
    }
    const auto status = sys::wait_one(rt_startup().events, 0, 0, *deadline).status;
    if (status == STATUS_TIMED_OUT) {
        if (remaining)
            *remaining = {};
        return px_result(STATUS_OK);
    }
    if (remaining) {
        const auto end = sys::clock_now();
        if (end.status == STATUS_OK) {
            const auto left = end.value < *deadline ? *deadline - end.value : 0;
            const auto ns = libk::checked_multiply(left % frequency.value, uint64_t{1'000'000'000});
            if (ns)
                *remaining = {static_cast<int64_t>(left / frequency.value),
                              static_cast<int64_t>(*ns / frequency.value)};
        }
    }
    return px_result(status);
}

extern "C" int open(const char* path, int flags, ...) {
    if (!path || (flags & ~(3u | O_CREAT | O_TRUNC | O_EXCL | O_APPEND)) != 0 ||
        (flags & 3u) == 3u)
        return px_result(STATUS_BAD_ARGS);
    if (!*path) return px_result(STATUS_NOT_FOUND);
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        const auto mode = va_arg(args, int);
        va_end(args);
        if (mode != 0) {
            errno = ENOTSUP;
            return -1;
        }
    }
    const uint32_t mode = flags & 3u;
    if (mode == O_RDONLY && (flags & (O_CREAT | O_TRUNC | O_EXCL | O_APPEND)))
        return px_result(STATUS_BAD_ARGS);
    size_t fd{}, slot{};
    while (fd < fds.size() && fds[fd])
        ++fd;
    if (fd == fds.size())
        fds.push_back(nullptr);
    while (slot < opened.size() && !std::holds_alternative<std::monostate>(opened[slot].data))
        ++slot;
    if (slot == opened.size())
        opened.emplace_back();
    uint64_t backend = mode == O_RDONLY   ? px_read
                       : mode == O_WRONLY ? px_write
                                          : px_read | px_write;
    if (flags & O_CREAT)
        backend |= px_create;
    if (flags & O_TRUNC)
        backend |= px_truncate;
    if (flags & O_EXCL)
        backend |= px_exclusive;
    if (flags & O_APPEND)
        backend |= px_append;
    cap_owner object;
    const auto absolute = px_path(path, cwd);
    auto status = files();
    uint64_t loan{};
    if (status == STATUS_OK) {
        ubus_msg query, reply;
        query.data.put("action", "lend").put("bytes", uint64_t{192 * 1024}).put("caps", uint64_t{0}).put("kinds", word_t{OBJ_BIT(ObjChannel) | OBJ_BIT(ObjMem)});
        status = dir_bus.invoke("Resource", std::move(query), reply);
        if (status != STATUS_OK) return px_result(status);
        loan = *reply.data["loan"].as<uint64_t>();
        auto quota = reply.take(reply.data["quota"]);
        query = {};
        const auto budget_idx = query.attach(quota.selector(), RIGHT_CREATE | RIGHT_INSPECT | RIGHT_DUPLICATE);
        if (!budget_idx) return px_result(budget_idx.error());
        query.data.put("path", absolute).put("flags", backend)
            .cap("budget", *budget_idx);
        status = dir_rpc.invoke("Open", std::move(query), reply); object = reply.take(reply.data["handle"]);
        if (status == STATUS_OK && !object) status = STATUS_PEER_FAULT;
    }
    if (status != STATUS_OK) {
        if (loan) { ubus_msg q, r; q.data.put("action", "close").put("loan", loan); (void)dir_bus.invoke("Resource", std::move(q), r); }
        return px_result(status);
    }
    auto& session = opened[slot].data.emplace<px_file>();
    status =
        session.open(std::move(object), mode != O_RDONLY, nullptr, loan);
    if (status != STATUS_OK) {
        opened[slot].data.emplace<std::monostate>();
        return px_result(status);
    }
    opened[slot].refs = 1;
    fds[fd] = &opened[slot];
    return px_result(fd);
}

extern "C" int32_t close(int32_t fd) {
    auto* value = desc(fd);
    if (!value)
        return px_result(STATUS_INVALID_CAP);
    const auto status = release(*value);
    if (status == STATUS_OK || std::holds_alternative<std::monostate>(value->data))
        fds[fd] = nullptr;
    return px_result(status);
}

extern "C" int32_t mkstemp(char* path) {
    if (!path)
        return px_result(STATUS_BAD_ARGS);
    const auto size = ::strlen(path);
    if (size < 6)
        return px_result(STATUS_BAD_ARGS);
    for (size_t i = size - 6; i < size; ++i)
        if (path[i] != 'X')
            return px_result(STATUS_BAD_ARGS);
    const auto now = sys::clock_now();
    if (now.status != STATUS_OK)
        return px_result(now.status);
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
        if (fd >= 0 || errno != EEXIST)
            return fd;
        candidate = (candidate + 1) % names;
    } while (candidate != start);
    return px_result(STATUS_BUSY);
}

extern "C" int dup(int fd) {
    auto* value = desc(fd);
    if (!value) {
        errno = EBADF;
        return -1;
    }
    size_t next{};
    while (next < fds.size() && fds[next])
        ++next;
    if (next == fds.size())
        fds.push_back(nullptr);
    fds[next] = value;
    ++value->refs;
    return next;
}

extern "C" ssize_t read(int fd, void* data, size_t size) {
    auto* value = desc(fd);
    if (!value || size > INT64_MAX || (size && !data))
        return px_result(STATUS_BAD_ARGS);
    if (auto* input = std::get_if<input_stream>(&value->data)) {
        if (size == 0)
            return px_result(0);
        if (!input->reader) {
            const auto status = input->reader.open(input->channel.selector(), events);
            if (status != STATUS_OK)
                return px_result(status);
        }
        auto bytes = input->frame.data["data"].as<std::span<const std::byte>>().value_or(std::span<const std::byte>{});
        while (input->offset == bytes.size()) {
            if (input->ended) return 0;
            const auto status = input->reader.receive(input->frame);
            if (status == STATUS_WOULD_BLOCK) { const auto wake = ev_wait(events).status; if (wake != STATUS_OK) return px_result(wake); continue; }
            if (status != STATUS_OK) return px_result(status);
            if (input->frame.status != STATUS_OK) return px_result(input->frame.status);
            bytes = input->frame.data["data"].as<std::span<const std::byte>>().value_or(std::span<const std::byte>{});
            input->ended = bytes.empty(); input->offset = 0;
        }
        const auto count = std::min(size, bytes.size() - input->offset);
        ::memcpy(data, bytes.data() + input->offset, count);
        input->offset += count;
        return px_result(count);
    }
    return px_result(file(fd) ? transfer(*value, px_op::Read, data, size, 0, false)
                              : STATUS_INVALID_CAP);
}

extern "C" ssize_t pread(int fd, void* data, size_t size, off_t offset) {
    if (offset < 0) {
        errno = EINVAL;
        return -1;
    }
    auto* opened = file(fd);
    return px_result(opened ? transfer(*opened, px_op::Read, data, size, offset, true)
                            : STATUS_INVALID_CAP);
}

extern "C" ssize_t pwrite(int fd, const void* data, size_t size, off_t offset) {
    if (offset < 0) {
        errno = EINVAL;
        return -1;
    }
    auto* opened = file(fd);
    return px_result(
        opened ? transfer(*opened, px_op::Write, const_cast<void*>(data), size, offset, true)
               : STATUS_INVALID_CAP);
}

extern "C" int32_t px_submit(const io_request* request, request_id* id) {
    if (!request || !id || (request->operation != IO_READ && request->operation != IO_WRITE))
        return STATUS_BAD_ARGS;
    auto* opened = file(request->fd);
    if (!opened)
        return STATUS_BAD_ARGS;
    pending* item{};
    const auto status =
        enqueue(*opened, static_cast<px_op>(request->operation), request->data,
                request->length, request->offset, false, false, item);
    if (status == STATUS_OK)
        *id = item->id;
    return status;
}

extern "C" int32_t px_completion(io_completion* result, uint32_t flags) {
    if (!result || (flags & ~IO_WAIT))
        return STATUS_BAD_ARGS;
    for (;;) {
        pump();
        bool active{};
        for (auto& item : requests)
            if (item.file && !item.internal) {
                active = true;
                if (item.ready) {
                    result->id = item.id;
                    result->result = take(item);
                    return 1;
                }
            }
        if (!active || !(flags & IO_WAIT))
            return 0;
        const auto status = wait();
        if (status != STATUS_OK)
            return status;
    }
}

extern "C" int32_t px_cancel(request_id id) {
    pump();
    for (auto& item : requests)
        if (item.file && item.id == id && !item.internal) {
            if (item.ready)
                return STATUS_BUSY;
            const auto status = item.file->queue().cancel(item.wire);
            return status == STATUS_NOT_FOUND ? STATUS_BUSY : status;
        }
    return STATUS_NOT_FOUND;
}

extern "C" int32_t fsync(int32_t fd) {
    auto* opened = file(fd);
    if (!opened)
        return px_result(STATUS_BAD_ARGS);
    if (opened->queue().error())
        return px_result(opened->queue().error());
    const auto drained = drain(opened);
    if (drained != STATUS_OK)
        return px_result(drained);
    if (opened->queue().error())
        return px_result(opened->queue().error());
    ubus_msg response;
    return px_result(opened->queue().invoke("Sync", {}, response));
}

extern "C" int32_t px_sync(void) {
    const auto status = drain();
    if (status != STATUS_OK)
        return status;
    const auto ready = files();
    ubus_msg reply; return ready == STATUS_OK ? dir_rpc.invoke("Sync", {}, reply) : ready;
}

extern "C" int32_t px_size(int32_t fd, uint64_t* size) {
    auto* opened = file(fd);
    if (!opened || !size)
        return STATUS_BAD_ARGS;
    if (opened->queue().error())
        return opened->queue().error();
    ubus_msg response;
    const auto status = opened->queue().invoke("Stat", {}, response);
    if (status != STATUS_OK) return status;
    const auto value = response.data["size"].as<uint64_t>();
    if (!value) return STATUS_PEER_FAULT;
    *size = *value; return STATUS_OK;
}

extern "C" int32_t px_mkdir(const char* path) {
    if (!path || !*path)
        return STATUS_BAD_ARGS;
    const auto absolute = px_path(path, cwd);
    const auto status = files();
    ubus_msg query, reply; query.data.put("path", absolute);
    return status == STATUS_OK ? dir_rpc.invoke("Mkdir", std::move(query), reply) : status;
}

extern "C" int chdir(const char* path) {
    auto* dir = opendir(path);
    if (!dir) return -1;
    const auto absolute = px_path(path, cwd);
    const int status = closedir(dir);
    if (!status) cwd = absolute;
    return status;
}
extern "C" char* getcwd(char* buffer, size_t size) {
    const auto bytes = cwd.size() + 1;
    if (!buffer && !size) size = bytes;
    if (size < bytes) { errno = ERANGE; return nullptr; }
    if (!buffer) buffer = static_cast<char*>(malloc(size));
    if (!buffer) { errno = ENOMEM; return nullptr; }
    ::memcpy(buffer, cwd.c_str(), bytes);
    return buffer;
}

extern "C" int32_t unlink(const char* path) {
    if (!path || !*path)
        return px_result(STATUS_BAD_ARGS);
    const auto absolute = px_path(path, cwd);
    const auto status = files();
    ubus_msg query, reply; query.data.put("path", absolute);
    return px_result(status == STATUS_OK ? dir_rpc.invoke("Remove", std::move(query), reply) : status);
}

extern "C" int32_t rename(const char* from, const char* to) {
    if (!from || !*from || !to || !*to)
        return px_result(STATUS_BAD_ARGS);
    const auto source = px_path(from, cwd), target = px_path(to, cwd);
    const auto status = files();
    ubus_msg query, reply; query.data.put("from", source).put("to", target);
    return px_result(status == STATUS_OK ? dir_rpc.invoke("Rename", std::move(query), reply) : status);
}

extern "C" ssize_t write(int fd, const void* data, size_t size) {
    if (auto* opened = file(fd))
        return px_result(
            transfer(*opened, px_op::Write, const_cast<void*>(data), size, 0, false));
    auto* value = desc(fd);
    auto* output = value ? std::get_if<output_stream>(&value->data) : nullptr;
    if (!output)
        return px_result(STATUS_BAD_ARGS);
    if ((size != 0 && data == nullptr) || size > INT64_MAX)
        return px_result(STATUS_BAD_ARGS);

    const auto* bytes = static_cast<const char*>(data);
    uint64_t done{};
    while (done < size) {
        const auto chunk = std::min(size - done, size_t{80});
        ubus_msg message; message.data.put("data", std::as_bytes(std::span{bytes + done, chunk}));
        const auto status = ubus_send(output->channel.selector(), message);
        if (status != STATUS_OK)
            return px_result(done != 0 ? static_cast<int64_t>(done) : status);
        done += chunk;
    }
    return px_result(done);
}
