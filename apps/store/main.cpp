#include <libk/path.hpp>
#include <vm.hpp>
#include <thread.hpp>
#include <string_view>
#include <string.h>
#include <utility>
#include "volume.hpp"
#include <start.hpp>
#include <libk/parse.hpp>


#include <expected>
#include <memory>
#include <new>
#include <deque>
#include <vector>


static ubus_object directory{"io.files"};
static uint64_t epoch{1};
static st_device backend;
static st_vol volume;
static std::array<uint8_t, io_limit> scratch;
static _ev_port* events;
static CreateAuth store_memory;
static _cap_desc descriptor;
static uint8_t expected_id[st_volume_id_size]{};
static bool require_id{};

static auto expected(const uint8_t* id) noexcept -> bool {
    for (size_t i = 0; i < sizeof(expected_id); ++i)
        if (id[i] != expected_id[i])
            return false;
    return true;
}

struct inode final : private libk::noncopyable_nonmovable {
    lfs_file_t state{};
    lfs_file_config config{};
    uint8_t cache[st_vol::BlockSize]{};
    std::unique_ptr<inode> next;
    size_t users{};
    bool open{};
    ~inode() {
        if (open)
            lfs_file_drop(volume.fs(), &state);
    }
};
static std::unique_ptr<inode> nodes;

static void drain_reads() noexcept;

static auto writeback(inode& node) noexcept -> status_t {
    if (node.state.flags & LFS_F_ERRED)
        return STATUS_BACKING_FAILED;
    if (node.state.flags & (LFS_F_DIRTY | LFS_F_WRITING)) ++epoch;
    return volume.error(lfs_file_sync(volume.fs(), &node.state));
}

static void reap() noexcept {
    for (auto* link = &nodes; *link;) {
        if ((*link)->users) {
            link = &(*link)->next;
            continue;
        }
        auto retired = std::move(*link);
        *link = std::move(retired->next);
    }
}

// littlefs updates these references on metadata relocation and deletion.
// Resolve namespace identity, never a second pathname/size cache.
struct entry {
    lfs_mdir_t dir;
    uint16_t id;
};
static auto lookup(const char* path) -> std::expected<entry, int> {
    uint8_t cache[st_vol::BlockSize];
    lfs_file_config config{};
    config.buffer = cache;
    lfs_file_t probe{};
    const int opened = lfs_file_opencfg(volume.fs(), &probe, path, LFS_O_RDONLY, &config);
    if (opened < 0)
        return std::unexpected(opened);
    const entry found{probe.m, probe.id};
    lfs_file_drop(volume.fs(), &probe);
    return found;
}
static auto find(const entry& entry) -> inode* {
    for (auto* node = nodes.get(); node; node = node->next.get())
        if (node->state.id == entry.id) {
            const auto& a = node->state.m.pair;
            const auto& b = entry.dir.pair;
            if (a[0] == b[0] || a[0] == b[1] || a[1] == b[0] || a[1] == b[1])
                return node;
        }
    return nullptr;
}

[[nodiscard]] static auto open_file(inode& file, const char* path, uint64_t flags) noexcept -> status_t {
    file.config = {};
    file.config.buffer = file.cache;
    const bool write = (flags & io_write) != 0;
    int mode = (flags & (io_read | io_write)) == (io_read | io_write) ? LFS_O_RDWR
               : write                                                ? LFS_O_WRONLY
                                                                      : LFS_O_RDONLY;
    if (flags & io_create)
        mode |= LFS_O_CREAT;
    if (flags & io_truncate)
        mode |= LFS_O_TRUNC;
    if (flags & io_exclusive)
        mode |= LFS_O_EXCL;
    const int result = lfs_file_opencfg(volume.fs(), &file.state, path, mode, &file.config);
    if (result < 0)
        return volume.error(result);
    file.open = true;
    return STATUS_OK;
}

[[nodiscard]] static auto transfer(inode& file, const st_cmd& request, uint8_t* data, uint64_t& logical,
                            bool implicit, bool readable, bool writable,
                            bool append = false) noexcept -> st_done {
    const bool write = request.operation == st_op::Write;
    if (!write && request.operation != st_op::Read)
        return {request.id, STATUS_INVALID_OP, 0};
    if ((write && !writable) || (!write && !readable))
        return {request.id, STATUS_DENIED, 0};
    auto offset = implicit ? logical : request.offset;
    if (write && implicit && append) {
        const auto size = lfs_file_size(volume.fs(), &file.state);
        if (size < 0)
            return {request.id, volume.error(size), 0};
        offset = size;
    }
    if (offset > LFS_FILE_MAX || request.length > LFS_FILE_MAX - offset)
        return {request.id, STATUS_BAD_ARGS, 0};
    const auto seek = lfs_file_seek(volume.fs(), &file.state, offset, LFS_SEEK_SET);
    if (seek < 0) {
        const auto status = volume.error(seek);
        if (volume.failed())
            _thread_exit(status);
        return {request.id, status, 0};
    }
    const auto result = write ? lfs_file_write(volume.fs(), &file.state, data, request.length)
                              : lfs_file_read(volume.fs(), &file.state, data, request.length);
    if (volume.failed())
        _thread_exit(volume.error(result));
    if (result > 0 && implicit)
        logical = offset + result;
    return {request.id, volume.error(result < 0 ? result : 0),
            result > 0 ? static_cast<uint64_t>(result) : 0};
}

struct st_open final {
    _vm_ptr<st_open> next;
    inode* file{};
    ubus_object object;
    ubus_queue<st_cmd, io_depth + 1> queue; // One control credit beyond the negotiated data depth.
    bool stopped{}, failed{}, closing{};
    void abort() { stopped = failed = true; queue.stop(); }
    auto finish(ubus_ticket ticket, status_t status, uint64_t count) -> bool {
        ubus_msg response; response.status = status;
        const auto kind = queue.request(ticket)->operation;
        if (kind == st_op::Read || kind == st_op::Write) response.data.put("bytes", count);
        else if (kind == st_op::Stat) response.data.put("size", count).put("sequence", queue.sequence()).put("limit", uint64_t{io_limit}).put("depth", uint64_t{io_depth});
        return queue.finish(ticket, std::move(response));
    }
    uint64_t offset{};
    bool busy{};
    bool readable{}, writable{}, append{};
    std::string document;
    auto save(ubus_request&) -> status_t;

    ~st_open() { if (file) --file->users; }

    [[nodiscard]] auto create(uint64_t flags, const char* path, ubus_msg& reply, cap_t budget) -> status_t {
        readable = flags & io_read; writable = flags & io_write; append = flags & io_append;
        status_t status = object.open(io_depth, budget);
        if (status == STATUS_OK) {
            const auto entry = lookup(path);
            if (!entry && entry.error() != LFS_ERR_NOENT)
                status = volume.error(entry.error());
            if (entry)
                file = find(*entry);
            if (file) {
                ++file->users;
                if ((flags & io_exclusive) && (flags & io_create))
                    status = io_exists;
                else if (flags & io_truncate) {
                    const auto result = lfs_file_truncate(volume.fs(), &file->state, 0);
                    status = volume.error(result);
                }
            } else if (status == STATUS_OK) {
                auto node = std::unique_ptr<inode>{new (std::nothrow) inode};
                if (!node)
                    return STATUS_NO_MEMORY;
                const auto mode = (flags & ~(io_read | io_write)) | io_read | io_write;
                status = open_file(*node, path, mode);
                if (status == STATUS_OK) {
                    file = node.get();
                    ++file->users;
                    node->next = std::move(nodes);
                    nodes = std::move(node);
                }
            }
        }
        if (status != STATUS_OK)
            return status;
        for (const auto& [name, kind] : std::array<std::pair<std::string_view, st_op>, 3>{{
            {"Stat", st_op::Stat}, {"Sync", st_op::Sync}, {"Close", st_op::Close}}}) {
            const auto output = kind == st_op::Stat ? ubus_schema{{"size", ubus_type::U64}, {"sequence", ubus_type::U64}, {"limit", ubus_type::U64}, {"depth", ubus_type::U64}} : ubus_schema{};
            if (status == STATUS_OK) status = object.add(std::string{name}, {{"id", ubus_type::U64, true}}, output,
                [this, kind](auto& q, auto&) {
                    if (stopped || closing) return STATUS_CLOSED;
                    if (queue.sequence() == UINT64_MAX) return STATUS_EXHAUSTED;
                    const auto id = q.data["id"].template as<uint64_t>().value_or(queue.sequence() + 1);
                    const auto result = queue.accept(id, {.id=id, .operation=kind, .deadline=q.deadline}, q);
                    if (kind == st_op::Close && result == STATUS_PENDING) closing = true;
                    return result;
                });
        }
        for (const auto& [name, kind] : std::array<std::pair<std::string_view, st_op>, 2>{{{"Read", st_op::Read}, {"Write", st_op::Write}}}) {
            if ((kind == st_op::Read && !readable) || (kind == st_op::Write && !writable)) continue;
            if (status == STATUS_OK) status = object.add(std::string{name},
                {{"id", ubus_type::U64}, {"offset", ubus_type::U64, true}, {"buffer", ubus_type::Buffer}}, {{"bytes", ubus_type::U64}},
                [this, kind](auto& q, auto&) {
                    if (stopped || closing) return STATUS_CLOSED;
                    if (queue.active() >= io_depth) return STATUS_BUSY;
                    const auto buffer = *q.data["buffer"].template as<ubus_buffer>();
                    const auto offset = q.data["offset"].template as<uint64_t>();
                    st_cmd command{.id=*q.data["id"].template as<uint64_t>(), .operation=kind, .offset=offset.value_or(0),
                        .buffer_offset=buffer.offset, .length=buffer.size, .sequential=!offset, .deadline=q.deadline};
                    if (!valid(command)) return STATUS_BAD_ARGS;
                    return queue.accept(command.id, command, q);
                });
        }
        if (status == STATUS_OK && !document.empty() && writable) status = object.add("Save", {{"buffer", ubus_type::Buffer}}, {},
            [this](auto& q, auto&) {
                if (stopped || closing) return STATUS_CLOSED;
                if (queue.sequence() == UINT64_MAX) return STATUS_EXHAUSTED;
                const auto buffer = *q.data["buffer"].template as<ubus_buffer>();
                if (buffer.size > LFS_FILE_MAX || buffer.offset > UINT64_MAX - buffer.size) return STATUS_BAD_ARGS;
                const auto id = queue.sequence() + 1;
                return queue.accept(id, {.id=id, .operation=st_op::Save, .deadline=q.deadline}, q);
            });
        if (status == STATUS_OK) status = object.add("Cancel", {{"id", ubus_type::U64}}, {}, [this](auto& q, auto&) {
            return queue.cancel(*q.data["id"].template as<uint64_t>());
        });
        if (status != STATUS_OK) return status;
        auto handle = object.handle(0, RIGHT_SEND);
        if (!handle) return handle.error();
        const auto handle_idx = reply.give(std::move(*handle));
        if (!handle_idx) return handle_idx.error();
        reply.data.cap("handle", *handle_idx);
        return STATUS_OK;
    }

    static auto valid(const st_cmd& request) noexcept -> bool {
        return request.length && request.length <= io_limit && request.buffer_offset <= UINT64_MAX - request.length;
    }
    auto transfer(ubus_ticket ticket) noexcept -> st_done {
        const auto& request = *queue.request(ticket);
        if (request.operation == st_op::Save) return {request.id, save(*queue.call(ticket))};
        if (request.operation == st_op::Stat) {
            const auto size = lfs_file_size(volume.fs(), &file->state);
            return {request.id, volume.error(size < 0 ? size : 0), size < 0 ? 0 : uint64_t(size)};
        }
        if (request.operation == st_op::Sync || request.operation == st_op::Close) {
            const auto status = writeback(*file);
            return {request.id, status == STATUS_OK ? backend.flush() : status};
        }
        auto* call = queue.call(ticket);
        const auto memory = call->capability(call->data["buffer"]);
        auto& bytes = scratch;
        if (request.operation == st_op::Write) {
            const auto status = _mem_read(memory, request.buffer_offset, std::as_writable_bytes(std::span{bytes}.first(request.length))).status;
            if (status != STATUS_OK) return {request.id, status};
            ++epoch;
        }
        auto result = ::transfer(*file, request, bytes.data(), offset, request.sequential, readable, writable, append);
        if (result.status == STATUS_OK && request.operation == st_op::Read)
            result.status = _mem_write(memory, request.buffer_offset, std::as_bytes(std::span{bytes}.first(result.bytes))).status;
        return result;
    }
};
static _vm_ptr<st_open> opens;
static std::deque<ubus_request> syncs;
static st_open* sync_head;
static std::vector<std::pair<st_open*, uint64_t>> sync_targets;
static bool sync_captured;

// The ticket retains its endpoint and inode until the device stops accessing
// the payload. Filesystem mutation is serialized behind these physical reads.
struct read_job final {
    uint64_t object{}, offset{}, deadline{};
    uint8_t* output{};
    size_t size{}, done{};
    void* context{};
    void (*complete)(read_job&, status_t) noexcept {};
    bool (*cancelled)(const read_job&) noexcept {};
    bool active{}, submitted{};
};

struct pending {
    st_open* open{};
    ubus_ticket ticket{};
    read_job read{};
    std::array<uint8_t, io_limit> payload;
};
static std::array<pending, io_depth> reads;
static auto map_read(const read_job& read) noexcept -> std::expected<st_extent, status_t> {
    auto& node = *reinterpret_cast<inode*>(read.object);
    return volume.extent(node.state, read.offset + read.done, read.size - read.done);
}
// CTZ lookup may itself read metadata. Leave one of the 32 device credits
// available so lookup never waits for a credit held by its own data reader.
// Filesystem mapping stays with its owner. This reader only schedules bounded
// physical reads and copies the authorized bytes from each aligned extent.
// Exact-id completion leaves metadata I/O and other owners' results untouched.
class st_reader final {
    struct flight {
        read_job* read{};
        uint64_t id{}, buffer{};
        st_extent extent{};
    };
    std::array<flight, io_depth - 1> flights_{};

    static auto cancelled(const read_job& read) noexcept -> bool {
        return read.cancelled && read.cancelled(read);
    }
    static void finish(read_job& read, status_t status) noexcept {
        read.active = read.submitted = false;
        read.complete(read, status);
    }
    void complete(flight& slot, const st_done& result) noexcept {
        const auto flight = slot;
        slot = {};
        auto& read = *flight.read;
        read.submitted = false;
        if (cancelled(read))
            finish(read, STATUS_CANCELED);
        else if (result.status != STATUS_OK)
            finish(read, result.status);
        else if (result.bytes != flight.extent.size)
            finish(read, STATUS_PEER_FAULT);
        else {
            const auto copied = backend.copy(flight.buffer + flight.extent.skip,
                {reinterpret_cast<std::byte*>(read.output + read.done), flight.extent.bytes});
            if (copied != STATUS_OK) { finish(read, copied); return; }
            read.done += flight.extent.bytes;
            if (read.done == read.size)
                finish(read, STATUS_OK);
        }
    }

  public:
    auto poll() noexcept -> std::expected<bool, status_t> {
        bool progress{};
        for (auto& slot : flights_)
            if (slot.read) {
                st_done result{};
                const auto status = backend.completion(result, slot.id);
                if (status == STATUS_WOULD_BLOCK)
                    continue;
                if (status != STATUS_OK)
                    return std::unexpected(status);
                complete(slot, result);
                progress = true;
            }
        return progress;
    }
    auto wait() noexcept -> status_t {
        for (auto& slot : flights_)
            if (slot.read) {
                st_done result{};
                const auto status = backend.wait(result, slot.id);
                if (status == STATUS_OK)
                    complete(slot, result);
                return status;
            }
        return STATUS_WOULD_BLOCK;
    }
    auto submit(read_job& read) noexcept -> std::expected<bool, status_t> {
        if (!read.active || read.submitted)
            return false;
        if (cancelled(read)) {
            finish(read, STATUS_CANCELED);
            return true;
        }
        if (const auto status = _deadline_status(read.deadline); status != STATUS_OK) {
            finish(read, status);
            return true;
        }
        if (read.done == read.size) {
            finish(read, STATUS_OK);
            return true;
        }
        for (auto& slot : flights_)
            if (!slot.read) {
                const auto st_extent = map_read(read);
                if (!st_extent) {
                    finish(read, st_extent.error());
                    return true;
                }
                if (!st_extent->bytes || st_extent->bytes > read.size - read.done ||
                    st_extent->skip > st_extent->size ||
                    st_extent->bytes > st_extent->size - st_extent->skip)
                    return std::unexpected(STATUS_INTERNAL);
                st_cmd request{.operation = st_op::Read,
                               .offset = st_extent->offset,
                               .length = st_extent->size,
                               .deadline = read.deadline};
                const auto status = backend.submit(request);
                if (status == STATUS_BUSY)
                    return false;
                if (status != STATUS_OK) {
                    finish(read, status);
                    return true;
                }
                read.submitted = true;
                slot = {&read, request.id, request.buffer_offset, *st_extent};
                return true;
            }
        return false;
    }
};

static st_reader reader;

static auto cancelled(const read_job& read) noexcept -> bool {
    const auto& pending = *static_cast<const ::pending*>(read.context);
    auto& session = *pending.open;
    return session.failed || session.queue.cancelled(pending.ticket);
}
static void completed(read_job& read, status_t status) noexcept {
    auto& pending = *static_cast<::pending*>(read.context);
    auto& open = *pending.open;
    auto& queue = open.queue;
    const auto* request = queue.request(pending.ticket);
    if (open.failed || queue.cancelled(pending.ticket)) status = STATUS_CANCELED;
    if (status == STATUS_OK) {
        auto* call = queue.call(pending.ticket);
        status = _mem_write(call->capability(call->data["buffer"]), request->buffer_offset,
            std::as_bytes(std::span{pending.payload}.first(read.done))).status;
    }
    if (request->sequential) {
        if (status == STATUS_OK) open.offset += read.done;
        open.busy = false;
    }
    if (!open.finish(pending.ticket, status, status == STATUS_OK ? read.done : 0)) _thread_exit(STATUS_INTERNAL);
    pending.open = nullptr; pending.read = {};
}
static auto slot() noexcept -> pending* {
    for (auto& pending : reads)
        if (!pending.open)
            return &pending;
    return nullptr;
}
static auto reading() noexcept -> bool {
    for (const auto& pending : reads)
        if (pending.open)
            return true;
    return false;
}
static auto pump_reads() noexcept -> bool {
    const auto polled = reader.poll();
    if (!polled)
        _thread_exit(polled.error());
    bool progress = *polled;
    for (auto& pending : reads) {
        const auto submitted = reader.submit(pending.read);
        if (!submitted)
            _thread_exit(submitted.error());
        progress |= *submitted;
    }
    return progress;
}
static void drain_reads() noexcept {
    while (reading()) {
        (void) pump_reads();
        if (reading())
            if (const auto rc = reader.wait(); rc != STATUS_OK)
                _thread_exit(rc);
    }
}
// The capability designates the selected directory entry. Replacement preserves
// other open descriptions; an external rename/replacement invalidates this one.
auto st_open::save(ubus_request& request) -> status_t {
    drain_reads();
    const auto selected = lookup(document.c_str());
    if (!selected) return STATUS_STALE;
    if (find(*selected) != file) return STATUS_STALE;
    const auto buffer = *request.data["buffer"].as<ubus_buffer>();
    if (buffer.size > LFS_FILE_MAX || buffer.offset > UINT64_MAX - buffer.size) return STATUS_BAD_ARGS;
    static uint64_t serial;
    const auto slash = document.rfind('/');
    std::string temporary;
    auto replacement = std::make_unique<inode>();
    status_t status;
    do {
        temporary = document.substr(0, slash + 1) + ".edit-" + std::to_string(++serial);
        status = open_file(*replacement, temporary.c_str(), io_read | io_write | io_create | io_exclusive);
    } while (status == io_exists);
    if (status != STATUS_OK) return status;
    auto bytes = std::as_writable_bytes(std::span{scratch});
    for (uint64_t at = 0; at < buffer.size && status == STATUS_OK;) {
        const auto count = std::min<uint64_t>(bytes.size(), buffer.size - at);
        status = _mem_read(request.capability(request.data["buffer"]), buffer.offset + at, std::span{bytes}.first(count)).status;
        if (status != STATUS_OK) break;
        const auto written = lfs_file_write(volume.fs(), &replacement->state, bytes.data(), count);
        status = written < 0 ? volume.error(written) : uint64_t(written) == count ? STATUS_OK : STATUS_BACKING_FAILED;
        at += count;
    }
    if (status == STATUS_OK) status = writeback(*replacement);
    if (status == STATUS_OK) status = backend.flush();
    if (status != STATUS_OK) {
        replacement.reset();
        (void)lfs_remove(volume.fs(), temporary.c_str());
        return status;
    }
    const auto renamed = lfs_rename(volume.fs(), temporary.c_str(), document.c_str());
    if (renamed < 0) {
        replacement.reset(); (void)lfs_remove(volume.fs(), temporary.c_str()); return volume.error(renamed);
    }
    const auto entry = lookup(document.c_str());
    if (!entry) return volume.error(entry.error());
    replacement->state.m = entry->dir; replacement->state.id = entry->id;
    --file->users; file = replacement.get(); ++file->users;
    replacement->next = std::move(nodes); nodes = std::move(replacement);
    ++epoch; reap();
    return backend.flush();
}

static auto start_read(st_open& open, ubus_ticket ticket, pending& pending) noexcept -> bool {
    const auto& request = *open.queue.request(ticket);
    auto& file = open.file->state;
    const auto offset = request.sequential ? open.offset : request.offset;
    if (request.operation != st_op::Read || !open.readable ||
        !st_open::valid(request) || offset > LFS_FILE_MAX || request.length > LFS_FILE_MAX - offset ||
        (file.flags & (LFS_F_INLINE | LFS_F_WRITING)))
        return false;
    if (const auto rc = backend.drain_writes(); rc != STATUS_OK)
        _thread_exit(rc);
    const auto available = offset < file.ctz.size ? file.ctz.size - offset : 0;
    pending.open = &open; pending.ticket = ticket;
    pending.read = {.object = reinterpret_cast<uintptr_t>(open.file), .offset = offset,
        .deadline = request.deadline, .output = pending.payload.data(), .size = std::min<uint64_t>(request.length, available),
        .context = &pending, .complete = completed, .cancelled = cancelled, .active = true};
    if (request.sequential)
        open.busy = true;
    return true;
}

// One server execution owns inode mutation. Map observes accepted content;
// its sealed snapshot is independent of later writes and provider retirement.
static auto snapshot(const char* path, word_t perms, ubus_msg& response) -> status_t {
    if (perms != VM_READ && perms != (VM_READ | VM_EXECUTE)) return STATUS_BAD_ARGS;
    const auto entry = lookup(path);
    if (!entry)
        return volume.error(entry.error());
    auto* source = find(*entry);
    std::unique_ptr<inode> temporary;
    if (!source) {
        temporary.reset(new (std::nothrow) inode);
        if (!temporary)
            return STATUS_NO_MEMORY;
        source = temporary.get();
        const auto status = open_file(*source, path, io_read);
        if (status != STATUS_OK)
            return status;
    }
    const auto size = lfs_file_size(volume.fs(), &source->state);
    if (size <= 0)
        return size < 0 ? volume.error(size) : STATUS_BAD_ARGS;
    const auto bytes = (uint64_t(size) + 4095) & ~uint64_t{4095};
    auto made = _vm_area::create(store_memory, _vm(), 0, bytes, VM_READ | VM_WRITE | (perms & VM_EXECUTE), VM_READ | VM_WRITE);
    if (!made) return made.error();
    auto staging = std::move(*made);
    status_t status = STATUS_OK;
    const auto position = lfs_file_tell(volume.fs(), &source->state);
    if (position < 0)
        return volume.error(position);
    status = backend.drain_writes();
    if (status != STATUS_OK)
        return status;
    auto copied = lfs_file_seek(volume.fs(), &source->state, 0, LFS_SEEK_SET);
    if (copied >= 0)
        copied = lfs_file_read(volume.fs(), &source->state,
                               reinterpret_cast<void*>(staging.address), size);
    const auto restored = lfs_file_seek(volume.fs(), &source->state, position, LFS_SEEK_SET);
    if (copied < 0 || restored < 0)
        return volume.error(copied < 0 ? copied : restored);
    if (copied != size)
        return STATUS_BACKING_FAILED;
    ::memset(reinterpret_cast<void*>(staging.address + size), 0, bytes - size);
    status = staging.detach();
    if (status == STATUS_OK)
        status = _memory_seal(staging.memory.selector()).status;
    if (status != STATUS_OK)
        return status;
    const auto view = _cap_mem(RIGHT_MAP | RIGHT_DUPLICATE | RIGHT_DELEGATE, 0, bytes / 4096, perms);
    auto exported = descriptor.derive(staging.memory.selector(), view, store_memory.quota);
    if (!exported) return exported.error();
    const auto index = response.offer(std::move(*exported), view.rights); if (!index) return STATUS_NO_MEMORY;
    response.data.put("buffer", ubus_buffer{*index, 0, uint64_t(size)}); return STATUS_OK;
}

static auto control(std::string_view method, ubus_request& request, ubus_msg& response) -> status_t {
    drain_reads();
    if (const auto status = _deadline_status(request.deadline); status != STATUS_OK) return status;
    if (method == "DeviceId") {
        uint8_t id[20]{};
        const auto status = backend.identify(id);
        if (status == STATUS_OK) response.data.put("id", std::as_bytes(std::span{id}));
        return status;
    }
    if (method == "VolumeId") {
        uint8_t id[st_volume_id_size]{};
        const auto status = volume.identity(id);
        if (status == STATUS_OK) response.data.put("id", std::as_bytes(std::span{id}));
        return status;
    }
    if (method == "Format") {
        const auto id = request.data["id"].as<std::span<const std::byte>>();
        if (id && id->size() != st_volume_id_size) return STATUS_BAD_ARGS;
        if (require_id && (!id || !expected(reinterpret_cast<const uint8_t*>(id->data())))) return STATUS_DENIED;
        if (opens) return STATUS_BUSY;
        nodes.reset(); ++epoch;
        return volume.format(id ? reinterpret_cast<const uint8_t*>(id->data()) : nullptr);
    }
    if (!volume.mounted()) return STATUS_BACKING_FAILED;
    if (method == "Sync") {
        if (syncs.size() >= io_depth) return STATUS_BUSY;
        if (syncs.empty()) sync_head = opens.get();
        syncs.push_back(std::move(request)); return STATUS_PENDING;
    }
    const auto raw = request.data[method == "Rename" ? "from" : "path"].as<std::string_view>();
    if (!raw || raw->find(char{}) != std::string_view::npos) return STATUS_BAD_ARGS;
    const auto path = libk::path_normal(*raw);
    if (method == "Map") {
        const auto perms = *request.data["perms"].as<word_t>();
        if ((perms & VM_EXECUTE) && !(request.facet() & io_execute)) return STATUS_DENIED;
        return snapshot(path.c_str(), perms, response);
    }
    if (method == "Open" || method == "Document") {
        const auto flags = method == "Document" ? uint64_t(io_read | ((request.facet() & io_modify) && request.data["writable"].as<bool>().value_or(false) ? io_write | io_create : 0))
            : *request.data["flags"].as<uint64_t>();
        if (!(flags & (io_read | io_write)) || flags & ~(io_read | io_write | io_create | io_truncate | io_exclusive | io_append)
            || ((flags & ~(io_read | io_write)) && !(flags & io_write))) return STATUS_BAD_ARGS;
        if ((flags & io_write) && !(request.facet() & io_modify)) return STATUS_DENIED;
        const auto budget = request.capability(request.data["budget"]);
        auto storage = _vm_new<st_open>({budget ? budget : _startup().quota, _startup().scope});
        if (!storage) return storage.error();
        auto opened = std::move(*storage);
        if (method == "Document") opened->document = path;
        const auto status = opened->create(flags, path.c_str(), response, request.capability(request.data["budget"]));
        if (status != STATUS_OK) return status;
        if (flags & (io_create | io_truncate)) ++epoch;
        opened->next = std::move(opens); opens = std::move(opened);
        return STATUS_OK;
    }
    if (method == "List") {
        const auto state = request.data["cursor"];
        uint64_t index{};
        if (state && !state.data().empty()) {
            const auto seen = state["epoch"].as<uint64_t>(), position = state["index"].as<uint64_t>();
            if (!seen || !position || *position > UINT32_MAX) return STATUS_BAD_ARGS;
            if (*seen != epoch) return STATUS_STALE;
            index = *position;
        }
        lfs_dir_t cursor{};
        int result = lfs_dir_open(volume.fs(), &cursor, path.c_str());
        if (result < 0) return volume.error(result);
        if (index) result = lfs_dir_seek(volume.fs(), &cursor, index);
        ubus_data items{ubus_type::Array};
        bool more{};
        for (size_t count = 0; result >= 0 && count < 64;) {
            lfs_info entry{};
            result = lfs_dir_read(volume.fs(), &cursor, &entry);
            if (result <= 0) break;
            if (std::string_view{entry.name} == "." || std::string_view{entry.name} == "..") continue;
            ubus_data item;
            item.put("name", entry.name).put("type", entry.type == LFS_TYPE_DIR ? "dir" : "file").put("size", uint64_t(entry.size));
            items.push(item); ++count;
            if (count == 64) more = true;
        }
        const auto next = lfs_dir_tell(volume.fs(), &cursor);
        const auto closed = lfs_dir_close(volume.fs(), &cursor);
        if (result < 0 || next < 0 || closed < 0) return volume.error(result < 0 ? result : next < 0 ? next : closed);
        response.data.put("epoch", epoch).put("items", items);
        if (more) { ubus_data next_cursor; next_cursor.put("epoch", epoch).put("index", uint64_t(next)); response.data.put("cursor", next_cursor); }
        return STATUS_OK;
    }
    int result{};
    ++epoch;
    if (method == "Remove") result = lfs_remove(volume.fs(), path.c_str());
    else if (method == "Mkdir") result = lfs_mkdir(volume.fs(), path.c_str());
    else if (method == "Rename") {
        const auto target = *request.data["to"].as<std::string_view>();
        if (target.find(char{}) != std::string_view::npos) return STATUS_BAD_ARGS;
        const auto to = libk::path_normal(target);
        const auto source = lookup(path.c_str());
        auto moved = source ? find(*source) : nullptr;
        if (!source && source.error() != LFS_ERR_ISDIR) result = source.error();
        else {
            result = lfs_rename(volume.fs(), path.c_str(), to.c_str());
            if (result == 0 && moved) {
                const auto entry = lookup(to.c_str());
                if (!entry) result = entry.error();
                else { moved->state.m = entry->dir; moved->state.id = entry->id; }
            }
        }
    } else return STATUS_INVALID_OP;
    return volume.error(result);
}

int main(int argc, char** argv) {
    const auto& info = _startup();
    events = &_events();
    if (argc > 2) return STATUS_BAD_ARGS;
    if (argc == 2) {
        const std::string_view text{argv[1]}; if (text.size() != sizeof(expected_id) * 2) return STATUS_BAD_ARGS;
        for (size_t i = 0; i < sizeof(expected_id); ++i) {
            const auto byte = libk::parse<uint8_t>(text.substr(i * 2, 2), 16); if (!byte) return STATUS_BAD_ARGS; expected_id[i] = *byte;
        }
        require_id = true;
    }
    ubus_conn bus, block;
    auto status = bus.bind(info.bootstrap); if (status != STATUS_OK) return status;
    ubus_msg setup;
    status = bus.invoke("Receive", {}, setup); if (status != STATUS_OK) return status;
    const auto message = setup.data["message"];
    store_memory = {setup.capability(message["image.quota"]), setup.capability(message["image.scope"])};
    if (!store_memory.quota || !store_memory.scope) return STATUS_DENIED;
    ubus_msg query, response; query.data.put("name", "block");
    status = bus.invoke("Get", std::move(query), response);
    if (status == STATUS_OK) status = block.bind(response.capability(response.data["connector"]), "io.block", 1, 1);
    if (status == STATUS_OK) {
        query = {}; query.data.put("action", "lend").put("bytes", uint64_t{192 * 1024}).put("caps", uint64_t{0})
            .put("kinds", word_t{OBJ_BIT(ObjChannel) | OBJ_BIT(ObjMem)});
        status = bus.invoke("Resource", std::move(query), response);
        if (status == STATUS_OK) status = backend.connect(block, response.capability(response.data["quota"]));
    }
    if (status == STATUS_OK) status = volume.open(backend);
    if (status != STATUS_OK) return status;
    if (require_id && volume.mounted()) {
        uint8_t actual[st_volume_id_size]{};
        if (const auto rc = volume.identity(actual); rc != STATUS_OK) return rc;
        if (!expected(actual)) return STATUS_DENIED;
    }
    struct method { std::string_view name; ubus_schema input, output; word_t rights; };
    for (auto& item : std::array<method, 12>{{
        {"Map", {{"path", ubus_type::String}, {"perms", ubus_type::U64}}, {{"buffer", ubus_type::Buffer}}, io_lookup},
        {"Document", {{"path", ubus_type::String}, {"writable", ubus_type::Bool, true}, {"budget", ubus_type::Cap, true}}, {{"handle", ubus_type::Cap}}, io_lookup},
        {"Open", {{"path", ubus_type::String}, {"flags", ubus_type::U64}, {"budget", ubus_type::Cap, true}}, {{"handle", ubus_type::Cap}}, io_lookup},
        {"List", {{"path", ubus_type::String}, {"cursor", ubus_type::Record, true}}, {{"epoch", ubus_type::U64}, {"items", ubus_type::Array}, {"cursor", ubus_type::Record, true}}, io_lookup},
        {"Remove", {{"path", ubus_type::String}}, {}, io_modify},
        {"Mkdir", {{"path", ubus_type::String}}, {}, io_modify},
        {"Rename", {{"from", ubus_type::String}, {"to", ubus_type::String}}, {}, io_modify},
        {"Sync", {}, {}, io_modify},
        {"DeviceId", {}, {{"id", ubus_type::Bytes}}, io_lookup},
        {"VolumeId", {}, {{"id", ubus_type::Bytes}}, io_lookup},
        {"Format", {{"id", ubus_type::Bytes, true}}, {}, io_admin},
        {"Capacity", {}, {{"bytes", ubus_type::U64}}, io_lookup}
    }}) {
        const auto name = item.name;
        status = directory.add(std::string{name}, std::move(item.input), std::move(item.output),
            [name](auto& request, auto& response) {
                if (name == "Capacity") { response.data.put("bytes", backend.capacity()); return STATUS_OK; }
                const auto status = control(name, request, response);
                if (volume.failed()) _thread_exit(volume.error(-1));
                reap(); return status;
            }, item.rights);
        if (status != STATUS_OK) return status;
    }
    if ((status = directory.open()) != STATUS_OK) return status;
    for (const auto& [name, rights] : std::array<std::pair<std::string_view, word_t>, 2>{{
        {"files", io_lookup | io_modify | io_execute}, {"admin", io_admin}}}) {
        auto connector = directory.connector(rights); if (!connector) return connector.error();
        ubus_msg request, response;
        const auto peer_idx = request.attach(connector->selector(), RIGHT_SEND | RIGHT_DUPLICATE);
        if (!peer_idx) return peer_idx.error();
        request.data.put("name", name).put("interface", "io.files").put("version", uint64_t{1})
            .cap("connector", *peer_idx);
        status = bus.invoke("Publish", std::move(request), response); if (status != STATUS_OK) return status;
    }
    for (;;) {
        // Report the failed barrier before retiring the poisoned filesystem.
        // Its metadata/cache must never serve a later operation after I/O EIO.
        if (volume.failed()) _thread_exit(volume.error(0));
        if (const auto status = backend.error(); status != STATUS_OK) _thread_exit(status);
        bool again = pump_reads();
        for (size_t count = 0; count < io_depth; ++count) {
            const auto status = directory.poll();
            if (status == STATUS_WOULD_BLOCK) break;
            if (status != STATUS_OK) return status;
            again = true;
        }
        for (auto* opened = opens.get(); opened; opened = opened->next.get())
            for (size_t n = 0; n < io_depth; ++n) {
                const auto status = opened->object.poll(); if (status == STATUS_WOULD_BLOCK) break;
                if (status == STATUS_CLOSED || status == STATUS_PEER_CLOSED) { opened->abort(); break; }
                if (status != STATUS_OK) return status;
                again = true;
            }
        if (!syncs.empty() && !sync_captured) {
            // Each endpoint has at most io_depth native queued requests. The
            // preceding receive round covered every request older than Sync;
            // later arrivals can extend the cut, never starve it.
            for (auto* opened = sync_head; opened; opened = opened->next.get())
                sync_targets.emplace_back(opened, opened->queue.sequence());
            sync_captured = true;
        }
        for (auto* owner = opens.get(); owner; owner = owner->next.get()) {
            auto& opened = *owner;
            auto* queue = &opened.queue;
            if (opened.stopped) continue;
            for (size_t row = 0; row < io_depth && !opened.busy; ++row) {
                auto* pending = slot();
                if (!pending) break;
                ubus_ticket ticket{};
                if (queue->admit(ticket) != ubus_admission::Ready) break;
                auto status = queue->cancelled(ticket) ? STATUS_CANCELED : _deadline_status(queue->request(ticket)->deadline);
                if (status != STATUS_OK) {
                    if (!opened.finish(ticket, status, 0)) return STATUS_INTERNAL;
                    again = true; continue;
                }
                if (start_read(opened, ticket, *pending)) { again = true; continue; }
                drain_reads();
                status = queue->cancelled(ticket) ? STATUS_CANCELED : _deadline_status(queue->request(ticket)->deadline);
                if (status == STATUS_OK && !queue->commit(ticket)) status = STATUS_CANCELED;
                const auto completion = status == STATUS_OK ? opened.transfer(ticket) : st_done{ticket.id, status};
                if (queue->request(ticket)->operation == st_op::Close) opened.stopped = true;
                if (!opened.finish(ticket, completion.status, completion.bytes)) return STATUS_INTERNAL;
                again = true;
            }
        }
        again |= pump_reads();
        if (sync_captured && std::ranges::none_of(sync_targets, [](const auto& cut) { return cut.first->queue.pending(cut.second); })) {
            auto status = STATUS_OK;
            for (auto* node = nodes.get(); node && status == STATUS_OK; node = node->next.get()) status = writeback(*node);
            if (status == STATUS_OK) status = backend.flush();
            ubus_msg response; response.status = status;
            if (const auto sent = syncs.front().respond(std::move(response)); sent != STATUS_OK) return sent;
            syncs.pop_front(); sync_targets.clear(); sync_captured = false; sync_head = opens.get(); again = true;
        }
        for (auto* link = &opens; *link;) {
            auto& opened = **link;
            if (opened.failed && !opened.queue.active() && opened.file->users == 1) {
                drain_reads();
                auto& file = *opened.file;
                if (file.state.m.pair[0] != UINT32_MAX && file.state.m.pair[1] != UINT32_MAX
                    && (file.state.flags & (LFS_F_DIRTY | LFS_F_WRITING))) (void)writeback(file);
            }
            if (opened.stopped && !opened.queue.active() && syncs.empty()) {
                again = true;
                auto retired = std::move(*link); *link = std::move(retired->next);
            } else link = &opened.next;
        }
        reap();
        if (const auto rc = directory.arm(); rc != STATUS_OK) return rc;
        if (!again) if (const auto rc = _ev_wait(events).status; rc != STATUS_OK) return rc;
    }
}
