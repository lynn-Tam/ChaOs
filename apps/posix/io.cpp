#include <io.hpp>
#include <libk/path.hpp>
#include <string.h>
#include <utility>
#include <variant>
#include <optional>
#include <array>
#include <mutex>
#include <bit>
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
#include <thread.hpp>

static auto files() -> status_t;
enum class px_op { Read = 1, Write };
struct px_cmd { uint64_t id{}; px_op operation{}; uint64_t offset{}, length{}; bool sequential{}; };

struct open_desc;
struct px_call { px_cmd command; void* destination{}; size_t slot{}; ubus_call call; };
struct pending {
    std::shared_ptr<open_desc> file;
    std::unique_ptr<px_call> operation;
    request_id id{};
    int64_t result{};
    bool ready{}, internal{};
};
struct px_context {
    ubus_conn rpc, bus;
    std::array<pending, IO_DEPTH> requests;
    request_id next{};
    ubus_tx output;
    _cleanup cleanup;
    ~px_context();
};
static thread_local px_context* px_current;
static auto px_local() -> px_context& {
    if (!px_current) {
        px_current = new px_context;
        _cleanup_defer(&px_current->cleanup, [](void* pointer) { delete static_cast<px_context*>(pointer); px_current = nullptr; }, px_current);
    }
    return *px_current;
}

// An open description shares only immutable authority and buffer slots. Each
// in-flight call belongs to its submitting thread, including its Reply/Port.
class px_session {
    _cap_owner endpoint_;
    _vm_area buffer_;
    _cap_desc descriptor_;
    _cap_owner exported_;
    _mutex submit_;
    uint32_t slots_{};
    uint64_t next_{}, loan_{};
    size_t limit_{}, depth_{}, stride_{};
    status_t error_{};
    uint64_t access_{};
    bool closed_{};
    static constexpr std::array<std::string_view, 6> names{"Read", "Write", "Cancel", "Close", "Sync", "Stat"};
    std::array<word_t, names.size()> tokens_{};
public:
    ~px_session() {
        if (exported_) (void)_cap_revoke(exported_.selector());
        auto memory = std::move(buffer_.memory);
        if (const auto status = buffer_.close(); status != STATUS_OK) _thread_exit(status);
        if (memory) (void)_object_destroy(memory.selector());
        if (loan_) {
            // This may run after the exiting thread's TLS connections died.
            ubus_conn bus;
            if (bus.bind(_startup().bootstrap) == STATUS_OK) {
                ubus_msg q, r; q.data.put("action", "close").put("loan", loan_);
                (void)bus.invoke("Resource", std::move(q), r);
            }
        }
    }
    auto open(_cap_owner endpoint, uint64_t access, uint64_t loan) -> status_t {
        loan_ = loan;
        ubus_conn peer;
        auto status = peer.adopt(std::move(endpoint)); if (status != STATUS_OK) return status;
        ubus_msg info; status = peer.invoke("Stat", {}, info); if (status != STATUS_OK) return status;
        const auto sequence = info.data["sequence"].as<uint64_t>(), size = info.data["size"].as<uint64_t>();
        const auto limit = info.data["limit"].as<uint64_t>(), depth = info.data["depth"].as<uint64_t>();
        if (!sequence || !size || !limit || !*limit || *limit > io_limit || !depth || !*depth || *depth > io_depth) return STATUS_PEER_FAULT;
        next_ = *sequence; limit_ = *limit; depth_ = *depth; stride_ = (limit_ + 4095) & ~size_t{4095};
        for (size_t i = 0; i < names.size(); ++i) {
            tokens_[i] = peer.token(names[i]).value_or(0);
        }
        auto authority = peer.release(); if (!authority) return authority.error();
        endpoint_ = std::move(*authority);
        auto wired = _vm(); wired.pager = 0;
        auto memory = _vm_area::create(_startup().alloc(), wired, 0, depth_ * stride_);
        if (!memory) return memory.error();
        buffer_ = std::move(*memory);
        const auto view = _cap_mem(RIGHT_MAP | RIGHT_DUPLICATE | RIGHT_REVOKE, 0, depth_ * stride_ / 4096, VM_READ | VM_WRITE);
        auto grant = descriptor_.derive(buffer_.memory.selector(), view); if (!grant) return grant.error();
        exported_ = std::move(*grant); access_ = access & (io_read | io_write); return STATUS_OK;
    }
    auto limits() const -> io_limits { return {limit_, depth_}; }
    auto submit(px_call& local, px_cmd command, void* data) -> status_t {
        std::lock_guard lock{submit_};
        if (closed_) return STATUS_CLOSED;
        if (error_) return error_;
        if (!endpoint_ || next_ == UINT64_MAX || command.length > limit_ || command.offset > UINT64_MAX - command.length) return STATUS_BAD_ARGS;
        if (!(access_ & (command.operation == px_op::Read ? io_read : io_write))) return STATUS_DENIED;
        if (size_t(std::popcount(slots_)) == depth_) return STATUS_BUSY;
        const auto kind = size_t(command.operation);
        if (!tokens_[kind - 1]) return STATUS_INVALID_OP;
        local.slot = std::countr_one(slots_); command.id = ++next_;
        const auto offset = local.slot * stride_;
        local.command = command; local.destination = data;
        if (command.operation == px_op::Write && command.length)
            ::memcpy(reinterpret_cast<void*>(buffer_.address + offset), data, command.length);
        ubus_msg message; message.tag = tokens_[kind - 1]; message.data.put("id", command.id);
        const auto cap = message.attach(exported_.selector(), RIGHT_MAP | RIGHT_DUPLICATE); if (!cap) return cap.error();
        message.data.put("buffer", ubus_buffer{*cap, offset, command.length});
        if (!command.sequential) message.data.put("offset", command.offset);
        auto status = local.call.begin(endpoint_.selector(), std::move(message), true);
        // Keep ID assignment and actual Channel acceptance in the same order.
        if (status == STATUS_OK) status = local.call.commit();
        if (status == STATUS_OK) slots_ |= uint32_t{1} << local.slot;
        else (void)local.call.cancel();
        return status;
    }
    auto completion(px_call& local) -> std::optional<int64_t> {
        if (!local.call.pending()) return {};
        ubus_msg response;
        const auto status = local.call.poll(response); if (status == STATUS_WOULD_BLOCK) return {};
        auto outcome = status == STATUS_OK ? response.status : status;
        const auto count = response.data["bytes"].as<uint64_t>();
        if (outcome == STATUS_OK && (!count || *count > local.command.length)) outcome = STATUS_PEER_FAULT;
        const auto bytes = outcome == STATUS_OK ? *count : 0;
        if (bytes && local.destination && local.command.operation != px_op::Write)
            ::memcpy(local.destination, reinterpret_cast<const void*>(buffer_.address + local.slot * stride_), bytes);
        {
            std::lock_guard lock{submit_};
            if (status != STATUS_OK && !error_) {
                error_ = status;
                const auto revoked = _cap_revoke(exported_.selector()).status;
                if (revoked != STATUS_OK) _thread_exit(revoked);
            }
            slots_ &= ~(uint32_t{1} << local.slot);
        }
        return outcome == STATUS_OK ? int64_t(bytes) : int64_t(outcome);
    }
    auto call(std::string_view name, ubus_msg q, ubus_msg& r) const -> status_t {
        const auto found = std::ranges::find(names, name); if (found == names.end()) return STATUS_INVALID_OP;
        q.tag = tokens_[found - names.begin()]; if (!q.tag) return STATUS_INVALID_OP;
        ubus_call call;
        const auto status = call.begin(endpoint_.selector(), std::move(q));
        if (status != STATUS_OK) return status;
        const auto waited = call.wait(r); return waited == STATUS_OK ? r.status : waited;
    }
    auto invoke(std::string_view name, ubus_msg q, ubus_msg& r) -> status_t {
        std::lock_guard lock{submit_};
        if (closed_) return STATUS_CLOSED;
        if (name == "Close") closed_ = true;
        if (error_) return error_;
        if (next_ == UINT64_MAX) return STATUS_EXHAUSTED;
        q.data.put("id", ++next_);
        return call(name, std::move(q), r);
    }
    auto cancel(uint64_t id) const -> status_t { ubus_msg q, r; q.data.put("id", id); return call("Cancel", std::move(q), r); }
};


extern "C" auto __errno() -> int* {
    static thread_local int value;
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
    case io_exists:
        errno = EEXIST;
        break;
    case io_not_dir:
        errno = ENOTDIR;
        break;
    case io_is_dir:
        errno = EISDIR;
        break;
    case io_not_empty:
        errno = ENOTEMPTY;
        break;
    case io_name_long:
        errno = ENAMETOOLONG;
        break;
    case io_too_large:
        errno = EFBIG;
        break;
    case io_cross_device:
        errno = EXDEV;
        break;
    case io_no_space:
        errno = ENOSPC;
        break;
    default:
        errno = EIO;
        break;
    }
    return -1;
}

static constinit _mutex table_mutex;
static std::shared_ptr<const std::string> cwd = std::make_shared<const std::string>("/");
static auto px_cwd() -> std::shared_ptr<const std::string> {
    std::lock_guard lock{table_mutex}; return cwd;
}
static auto px_absolute(std::string_view path) -> std::string { return libk::path_normal(path, *px_cwd()); }

struct directory {
    _mutex mutex;
    ubus_data page;
    std::string path;
    size_t index{};
    dirent entry{};
};

struct input_stream {
    _cap_owner channel;
    ubus_frame frame;
    bool ended{};
    uint64_t offset{};
};
struct output_stream {
    _cap_owner channel;
};
struct open_desc {
    size_t fds{}; // Table ownership is independent of in-flight I/O/completion references.
    _mutex sequential;
    std::variant<input_stream, output_stream, px_session> data;
    auto queue() noexcept -> px_session& { return std::get<px_session>(data); }
};
static std::vector<std::shared_ptr<open_desc>> fds(3);
static bool initialized;
static status_t init_status{};
static constinit _mutex init_mutex;
static auto files() -> status_t {
    if (px_local().rpc.endpoint()) {
        const auto fact = _source_info(px_local().rpc.endpoint(), SOURCE_WRITE);
        if (fact.status == STATUS_OK && !(fact.value & SOURCE_TERMINAL)) return STATUS_OK;
        (void)px_local().rpc.close();
    }
    if (!px_local().bus.endpoint()) {
        const auto status = px_local().bus.bind(_startup().bootstrap);
        if (status != STATUS_OK) return status;
    }
    ubus_msg request, response; request.data.put("name", "files");
    const auto status = px_local().bus.invoke("Get", std::move(request), response);
    return status == STATUS_OK ? px_local().rpc.bind(response.capability(response.data["connector"]), "io.files", 1, 1) : status;
}
static std::vector<std::shared_ptr<directory>> dirs;
static auto desc(int fd) noexcept -> std::shared_ptr<open_desc> {
    std::lock_guard lock{table_mutex};
    return fd >= 0 && static_cast<size_t>(fd) < fds.size() ? fds[fd] : nullptr;
}
static auto file(int fd) noexcept -> std::shared_ptr<open_desc> {
    auto value = desc(fd);
    return value && std::holds_alternative<px_session>(value->data) ? value : nullptr;
}
static auto px_install(std::shared_ptr<open_desc> value) -> int {
    ++value->fds;
    const auto slot = std::ranges::find(fds, nullptr);
    if (slot != fds.end()) { *slot = std::move(value); return slot - fds.begin(); }
    fds.push_back(std::move(value)); return fds.size() - 1;
}
static auto install(std::shared_ptr<open_desc> value) -> int {
    std::lock_guard lock{table_mutex}; return px_install(std::move(value));
}
static_assert(IO_DEPTH == io_depth && IO_LIMIT == io_limit);
static auto finish(pending&) noexcept -> int64_t;
px_context::~px_context() {
    for (auto& item : requests) if (item.file) {
        if (!item.ready) (void)item.file->queue().cancel(item.operation->command.id);
        (void)finish(item);
    }
}
static auto enqueue(std::shared_ptr<open_desc> file, px_op op, void* data, uint64_t size, uint64_t offset,
                    bool implicit, bool internal, pending*& out) noexcept -> int32_t {
    if (px_local().next == UINT64_MAX || (size && !data)) return STATUS_BAD_ARGS;
    for (auto& item : px_local().requests) if (!item.file) {
        // The submitting thread owns each slot's Reply/Port transport. Keep it
        // across completions instead of creating kernel objects for every I/O.
        if (!item.operation) item.operation = std::make_unique<px_call>();
        const px_cmd request{.operation=op, .offset=offset, .length=size, .sequential=implicit};
        const auto status = file->queue().submit(*item.operation, request, data); if (status != STATUS_OK) return status;
        item.file = std::move(file);
        item.id = ++px_local().next; item.internal = internal; out = &item; return STATUS_OK;
    }
    return STATUS_BUSY;
}
static void pump() noexcept {
    for (auto& item : px_local().requests) if (item.file && !item.ready) {
        if (const auto result = item.file->queue().completion(*item.operation)) {
            item.result = *result; item.ready = true;
        }
    }
}
static auto wait() noexcept -> int32_t { return _ev_wait(&_events()).status; }
static auto take(pending& item) noexcept -> int64_t {
    const auto result = item.result;
    item.file.reset(); item.id = 0; item.result = 0; item.ready = item.internal = false;
    return result;
}
static auto drain(const std::shared_ptr<open_desc>& target = {}) noexcept -> int32_t {
    for (;;) {
        pump(); bool active{};
        for (const auto& item : px_local().requests) if (item.file && (!target || item.file == target) && !item.ready) active = true;
        if (!active) return STATUS_OK;
        const auto status = wait(); if (status != STATUS_OK) return status;
    }
}
static auto finish(pending& item) noexcept -> int64_t {
    while (!item.ready) {
        pump(); if (item.ready) break;
        const auto status = wait(); if (status != STATUS_OK) _thread_exit(status);
    }
    return take(item);
}
static auto transfer(std::shared_ptr<open_desc> file, px_op op, void* data, uint64_t size, uint64_t offset,
                     bool explicit_offset) noexcept -> int64_t {
    if (size > INT64_MAX || (size && !data) || (explicit_offset && offset > UINT64_MAX - size))
        return STATUS_BAD_ARGS;
    uint64_t done{};
    while (done < size) {
        const auto count = std::min(size - done, file->queue().limits().bytes);
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
    auto dir = std::make_shared<directory>();
    dir->path = px_absolute(path);
    ubus_msg query, reply; query.data.put("path", dir->path);
    status = px_local().rpc.invoke("List", std::move(query), reply); dir->page = std::move(reply.data);
    if (status != STATUS_OK) { (void)px_result(status); return nullptr; }
    auto* result = dir.get();
    std::lock_guard lock{table_mutex};
    for (auto& slot : dirs) if (!slot) { slot = std::move(dir); return result; }
    dirs.push_back(std::move(dir)); return result;
}

extern "C" dirent* readdir(DIR* dir) {
    std::shared_ptr<directory> held;
    { std::lock_guard lock{table_mutex}; for (const auto& slot : dirs) if (slot.get() == dir) { held = slot; break; } }
    if (!held) { errno = EBADF; return nullptr; }
    std::lock_guard lock{held->mutex};
    const auto connected = files(); if (connected != STATUS_OK) { (void)px_result(connected); return nullptr; }
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
        const auto status = px_local().rpc.invoke("List", std::move(query), reply); page = std::move(reply.data);
        if (status != STATUS_OK) { (void)px_result(status); return nullptr; }
        dir->page = std::move(page); dir->index = 0;
    }
}

extern "C" int closedir(DIR* dir) {
    std::shared_ptr<directory> removed;
    { std::lock_guard lock{table_mutex}; for (auto& slot : dirs) if (slot && slot.get() == dir) { removed = std::move(slot); break; } }
    if (removed) return 0;
    errno = EBADF; return -1;
}

auto px_init(ubus_msg* received) noexcept -> status_t {
    std::lock_guard init{init_mutex};
    if (initialized) return STATUS_BUSY;
    if (init_status != STATUS_OK) return init_status;
    const auto& env = _startup();
    std::array<_cap_owner, 3> streams;
    auto status = px_local().bus.bind(env.bootstrap); if (status != STATUS_OK) return status;
    ubus_msg storage;
    auto& setup = received ? *received : storage;
    if (!received) { status = px_local().bus.invoke("Receive", {}, setup); if (status != STATUS_OK) return status; }
    init_status = STATUS_CLOSED; // Receive is consumed; a failed publication cannot repeat it.
    const std::array<std::string_view, 3> fields{"stdin", "stdout", "stderr"};
    for (size_t fd = 0; fd < streams.size(); ++fd) streams[fd] = setup.take(setup.data["message"][fields[fd]]);
    std::string initial_cwd{"/"};
    for (auto** value = environ; value && *value; ++value)
        if (const std::string_view entry{*value}; entry.starts_with("PWD=/")) {
            initial_cwd = libk::path_normal(entry.substr(4));
            break;
        }
    std::array<std::shared_ptr<open_desc>, 3> initial_fds;
    for (int fd = 0; fd < 3; ++fd) {
        if (!streams[fd])
            continue;
        auto value = std::make_shared<open_desc>();
        if (fd == 0)
            value->data.emplace<input_stream>().channel = std::move(streams[fd]);
        else
            value->data.emplace<output_stream>().channel = std::move(streams[fd]);
        value->fds = 1; initial_fds[fd] = std::move(value);
    }
    if (std::atexit([] {
        for (size_t fd = 0;; ++fd) {
            { std::lock_guard lock{table_mutex}; if (fd == fds.size()) break; }
            (void)::close(fd);
        }
    })) return init_status = STATUS_NO_MEMORY;
    auto path = std::make_shared<const std::string>(std::move(initial_cwd));
    { std::lock_guard lock{table_mutex}; cwd = std::move(path); std::move(initial_fds.begin(), initial_fds.end(), fds.begin()); }
    initialized = true; init_status = STATUS_OK;
    return STATUS_OK;
}

extern "C" int clock_gettime(clockid_t clock, timespec* time) {
    if (!time || (clock != CLOCK_MONOTONIC && clock != CLOCK_REALTIME)) return px_result(STATUS_BAD_ARGS);
    if (clock == CLOCK_REALTIME) return px_result(STATUS_INVALID_OP);
    const auto frequency = _clock_frequency();
    if (frequency.status != STATUS_OK)
        return px_result(frequency.status);
    const auto now = _clock_now();
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
    const auto ns = static_cast<unsigned __int128>(delay->tv_sec) * 1000000000 + delay->tv_nsec;
    if (ns > UINT64_MAX) return px_result(STATUS_BAD_ARGS);
    _sleep_ns(uint64_t(ns));
    if (remaining) *remaining = {};
    return 0;
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
    uint64_t backend = mode == O_RDONLY   ? io_read
                       : mode == O_WRONLY ? io_write
                                          : io_read | io_write;
    if (flags & O_CREAT)
        backend |= io_create;
    if (flags & O_TRUNC)
        backend |= io_truncate;
    if (flags & O_EXCL)
        backend |= io_exclusive;
    if (flags & O_APPEND)
        backend |= io_append;
    _cap_owner object;
    const auto absolute = px_absolute(path);
    auto status = files();
    uint64_t loan{};
    if (status == STATUS_OK) {
        ubus_msg query, reply;
        query.data.put("action", "lend").put("bytes", uint64_t{192 * 1024}).put("caps", uint64_t{0}).put("kinds", word_t{OBJ_BIT(ObjChannel) | OBJ_BIT(ObjMem)});
        status = px_local().bus.invoke("Resource", std::move(query), reply);
        if (status != STATUS_OK) return px_result(status);
        const auto offered = reply.data["loan"].as<uint64_t>();
        if (!offered || !*offered) return px_result(STATUS_PEER_FAULT);
        loan = *offered;
        auto quota = reply.take(reply.data["quota"]);
        query = {};
        const auto budget_idx = query.attach(quota.selector(), RIGHT_CREATE | RIGHT_INSPECT | RIGHT_DUPLICATE);
        if (!budget_idx) return px_result(budget_idx.error());
        query.data.put("path", absolute).put("flags", backend)
            .cap("budget", *budget_idx);
        status = px_local().rpc.invoke("Open", std::move(query), reply); object = reply.take(reply.data["handle"]);
        if (status == STATUS_OK && !object) status = STATUS_PEER_FAULT;
    }
    if (status != STATUS_OK) {
        if (loan) { ubus_msg q, r; q.data.put("action", "close").put("loan", loan); (void)px_local().bus.invoke("Resource", std::move(q), r); }
        return px_result(status);
    }
    auto value = std::make_shared<open_desc>();
    auto& session = value->data.emplace<px_session>();
    status = session.open(std::move(object), backend, loan);
    return status == STATUS_OK ? install(std::move(value)) : px_result(status);
}

extern "C" int32_t px_limits(int32_t fd, io_limits* limits) {
    const auto value = file(fd);
    if (!value) return STATUS_INVALID_CAP;
    if (!limits || !std::holds_alternative<px_session>(value->data)) return STATUS_BAD_ARGS;
    *limits = value->queue().limits(); return STATUS_OK;
}

extern "C" int32_t close(int32_t fd) {
    std::shared_ptr<open_desc> removed;
    bool last;
    { std::lock_guard lock{table_mutex};
      if (fd < 0 || size_t(fd) >= fds.size() || !fds[fd]) return px_result(STATUS_INVALID_CAP);
      removed = std::move(fds[fd]); last = !--removed->fds; }
    if (last && std::holds_alternative<px_session>(removed->data)) {
        ubus_msg response;
        return px_result(removed->queue().invoke("Close", {}, response));
    }
    return 0;
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
    const auto now = _clock_now();
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
    std::lock_guard lock{table_mutex};
    return fd < 0 || size_t(fd) >= fds.size() || !fds[fd] ? px_result(STATUS_INVALID_CAP) : px_install(fds[fd]);
}

extern "C" ssize_t read(int fd, void* data, size_t size) {
    auto value = desc(fd);
    if (!value) return px_result(STATUS_INVALID_CAP);
    if (size > INT64_MAX || (size && !data)) return px_result(STATUS_BAD_ARGS);
    std::lock_guard lock{value->sequential};
    if (auto* input = std::get_if<input_stream>(&value->data)) {
        if (size == 0)
            return px_result(0);
        while (input->offset == input->frame.size) {
            if (input->ended) return 0;
            const auto status = ubus_recv(input->channel.selector(), input->frame);
            if (status != STATUS_OK) return px_result(status);
            if (input->frame.status != STATUS_OK) return px_result(input->frame.status);
            input->ended = !input->frame.size; input->offset = 0;
        }
        const auto count = std::min(size, input->frame.size - input->offset);
        ::memcpy(data, input->frame.data + input->offset, count);
        input->offset += count;
        return px_result(count);
    }
    return px_result(std::holds_alternative<px_session>(value->data) ? transfer(value, px_op::Read, data, size, 0, false)
                              : STATUS_INVALID_CAP);
}

extern "C" ssize_t pread(int fd, void* data, size_t size, off_t offset) {
    if (offset < 0) {
        errno = EINVAL;
        return -1;
    }
    auto opened = file(fd);
    return px_result(opened ? transfer(opened, px_op::Read, data, size, offset, true)
                            : STATUS_INVALID_CAP);
}

extern "C" ssize_t pwrite(int fd, const void* data, size_t size, off_t offset) {
    if (offset < 0) {
        errno = EINVAL;
        return -1;
    }
    auto opened = file(fd);
    return px_result(
        opened ? transfer(opened, px_op::Write, const_cast<void*>(data), size, offset, true)
               : STATUS_INVALID_CAP);
}

extern "C" int32_t px_submit(const io_request* request, request_id* id) {
    if (!request || !id || (request->operation != IO_READ && request->operation != IO_WRITE))
        return STATUS_BAD_ARGS;
    auto opened = file(request->fd);
    if (!opened)
        return STATUS_BAD_ARGS;
    pending* item{};
    const auto status =
        enqueue(opened, static_cast<px_op>(request->operation), request->data,
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
        for (auto& item : px_local().requests)
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
    for (auto& item : px_local().requests)
        if (item.file && item.id == id && !item.internal) {
            if (item.ready)
                return STATUS_BUSY;
            const auto status = item.file->queue().cancel(item.operation->command.id);
            return status == STATUS_NOT_FOUND ? STATUS_BUSY : status;
        }
    return STATUS_NOT_FOUND;
}

extern "C" int32_t fsync(int32_t fd) {
    auto opened = file(fd);
    if (!opened) return px_result(STATUS_INVALID_CAP);
    ubus_msg response;
    return px_result(opened->queue().invoke("Sync", {}, response));
}

extern "C" int32_t px_sync(void) {
    const auto status = drain();
    if (status != STATUS_OK)
        return status;
    std::vector<std::shared_ptr<open_desc>> opened;
    { std::lock_guard lock{table_mutex};
      for (const auto& file : fds) if (file && std::holds_alternative<px_session>(file->data)
          && std::ranges::find(opened, file) == opened.end()) opened.push_back(file); }
    for (const auto& file : opened) {
        ubus_msg response; const auto status = file->queue().invoke("Sync", {}, response);
        if (status != STATUS_OK) return status;
    }
    const auto ready = files();
    ubus_msg reply; return ready == STATUS_OK ? px_local().rpc.invoke("Sync", {}, reply) : ready;
}

extern "C" int32_t px_size(int32_t fd, uint64_t* size) {
    auto opened = file(fd);
    if (!opened || !size)
        return STATUS_BAD_ARGS;
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
    const auto absolute = px_absolute(path);
    const auto status = files();
    ubus_msg query, reply; query.data.put("path", absolute);
    return status == STATUS_OK ? px_local().rpc.invoke("Mkdir", std::move(query), reply) : status;
}

extern "C" int chdir(const char* path) {
    auto* dir = opendir(path); if (!dir) return -1;
    auto absolute = std::make_shared<const std::string>(dir->path);
    const auto status = closedir(dir);
    if (!status) { std::lock_guard lock{table_mutex}; cwd = std::move(absolute); }
    return status;
}
extern "C" char* getcwd(char* buffer, size_t size) {
    const auto path = px_cwd();
    const auto bytes = path->size() + 1;
    if (!buffer && !size) size = bytes;
    if (size < bytes) { errno = ERANGE; return nullptr; }
    if (!buffer) buffer = static_cast<char*>(malloc(size));
    if (!buffer) { errno = ENOMEM; return nullptr; }
    ::memcpy(buffer, path->c_str(), bytes); return buffer;
}

extern "C" int32_t unlink(const char* path) {
    if (!path || !*path)
        return px_result(STATUS_BAD_ARGS);
    const auto absolute = px_absolute(path);
    const auto status = files();
    ubus_msg query, reply; query.data.put("path", absolute);
    return px_result(status == STATUS_OK ? px_local().rpc.invoke("Remove", std::move(query), reply) : status);
}

extern "C" int32_t rename(const char* from, const char* to) {
    if (!from || !*from || !to || !*to)
        return px_result(STATUS_BAD_ARGS);
    const auto base = px_cwd();
    const auto source = libk::path_normal(from, *base), target = libk::path_normal(to, *base);
    const auto status = files();
    ubus_msg query, reply; query.data.put("from", source).put("to", target);
    return px_result(status == STATUS_OK ? px_local().rpc.invoke("Rename", std::move(query), reply) : status);
}

extern "C" ssize_t write(int fd, const void* data, size_t size) {
    auto value = desc(fd);
    if (!value) return px_result(STATUS_INVALID_CAP);
    std::lock_guard lock{value->sequential};
    if (std::holds_alternative<px_session>(value->data))
        return px_result(transfer(value, px_op::Write, const_cast<void*>(data), size, 0, false));
    auto* output = value ? std::get_if<output_stream>(&value->data) : nullptr;
    if (!output)
        return px_result(STATUS_BAD_ARGS);
    if ((size != 0 && data == nullptr) || size > INT64_MAX)
        return px_result(STATUS_BAD_ARGS);

    const auto* bytes = static_cast<const char*>(data);
    uint64_t done{};
    while (done < size) {
        ubus_frame frame; frame.size = std::min(size - done, ubus_stream_limit);
        ::memcpy(frame.data, bytes + done, frame.size);
        const auto status = px_local().output.send(output->channel.selector(), frame);
        if (status != STATUS_OK)
            return px_result(done != 0 ? static_cast<int64_t>(done) : status);
        done += frame.size;
    }
    return px_result(done);
}

extern "C" int mkdir(const char* path, mode_t) { return px_result(px_mkdir(path)); }
