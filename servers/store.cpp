#include <vm.hpp>
#include <util.hpp>
#include <string_view>
#include <string.h>
#include <utility>
#include <lib/io/io.hpp>
#include <servers/volume.hpp>
#include <start.hpp>
#include <lib/fs/fs.hpp>


#include <expected>
#include <memory>
#include <new>

static ubus_object directory{"myos.files"};
static uint64_t epoch{1};
static blk_client backend;
static st_vol volume;
static cap_t events;
static CreateAuth store_memory;
static cap_desc descriptor;
static uint8_t expected_id[fs_volume_id_size]{};
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
    const auto status = volume.error(lfs_file_sync(volume.fs(), &node.state));
    if (volume.failed())
        sys::exit(status);
    return status;
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
    const bool write = (flags & fs_write) != 0;
    int mode = (flags & (fs_read | fs_write)) == (fs_read | fs_write) ? LFS_O_RDWR
               : write                                                ? LFS_O_WRONLY
                                                                      : LFS_O_RDONLY;
    if (flags & fs_create)
        mode |= LFS_O_CREAT;
    if (flags & fs_truncate)
        mode |= LFS_O_TRUNC;
    if (flags & fs_exclusive)
        mode |= LFS_O_EXCL;
    const int result = lfs_file_opencfg(volume.fs(), &file.state, path, mode, &file.config);
    if (result < 0)
        return volume.error(result);
    file.open = true;
    return STATUS_OK;
}

[[nodiscard]] static auto transfer(inode& file, const io_cmd& request, uint8_t* data, uint64_t& logical,
                            bool implicit, bool readable, bool writable,
                            bool append = false) noexcept -> io_done {
    const bool write = request.operation == io_operation::Write;
    if (!write && request.operation != io_operation::Read)
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
            sys::exit(status);
        return {request.id, status, 0};
    }
    const auto result = write ? lfs_file_write(volume.fs(), &file.state, data, request.length)
                              : lfs_file_read(volume.fs(), &file.state, data, request.length);
    if (volume.failed())
        sys::exit(volume.error(result));
    if (result > 0 && implicit)
        logical = offset + result;
    return {request.id, volume.error(result < 0 ? result : 0),
            result > 0 ? static_cast<uint64_t>(result) : 0};
}

struct Open final {
    std::unique_ptr<Open> next;
    inode* file{};
    io_server session;
    uint64_t offset{};
    bool busy{};
    bool readable{}, writable{}, append{};

    ~Open() { if (file) --file->users; }

    [[nodiscard]] auto create(uint64_t flags, const char* path, ubus_msg& reply) -> status_t {
        readable = flags & fs_read; writable = flags & fs_write; append = flags & fs_append;
        auto status = session.open(0, (readable ? uint64_t{io_read} : 0) | (writable ? uint64_t{io_write} : 0));
        if (status == STATUS_OK) {
            const auto entry = lookup(path);
            if (!entry && entry.error() != LFS_ERR_NOENT)
                status = volume.error(entry.error());
            if (entry)
                file = find(*entry);
            if (file) {
                ++file->users;
                if ((flags & fs_exclusive) && (flags & fs_create))
                    status = fs_exists;
                else if (flags & fs_truncate) {
                    const auto result = lfs_file_truncate(volume.fs(), &file->state, 0);
                    status = volume.error(result);
                }
            } else if (status == STATUS_OK) {
                auto node = std::unique_ptr<inode>{new (std::nothrow) inode};
                if (!node)
                    return STATUS_NO_MEMORY;
                const auto mode = (flags & ~(fs_read | fs_write)) | fs_read | fs_write;
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
        status = session.object().add("Stat", {}, {{"size", ubus_type::U64}}, [this](auto& request, auto& result) {
            drain_reads();
            if (const auto rc = rt_admit(request.deadline); rc != STATUS_OK) return rc;
            const auto size = lfs_file_size(volume.fs(), &file->state);
            if (size < 0) return volume.error(size);
            result.data.put("size", uint64_t(size)); return STATUS_OK;
        });
        if (status == STATUS_OK) status = session.object().add("Sync", {}, {}, [this](auto& request, auto&) {
            drain_reads();
            if (const auto rc = rt_admit(request.deadline); rc != STATUS_OK) return rc;
            const auto rc = writeback(*file);
            return rc == STATUS_OK ? backend.flush() : rc;
        });
        if (status != STATUS_OK) return status;
        auto handle = session.object().handle(0, RIGHT_SEND);
        if (!handle) return handle.error();
        reply.data.cap("handle", *reply.give(std::move(*handle)));
        return STATUS_OK;
    }

    static auto valid(const io_cmd& request) noexcept -> bool {
        return request.length && request.length <= io_buffer_size &&
               request.buffer_offset <= io_payload_size &&
               request.length <= io_payload_size - request.buffer_offset;
    }
    [[nodiscard]] auto transfer(const io_cmd& request) noexcept -> io_done {
        if (!valid(request))
            return {request.id, STATUS_BAD_ARGS, 0};
        if (request.operation == io_operation::Write) ++epoch;
        return ::transfer(*file, request, session.payload() + request.buffer_offset, offset,
                          request.sequential, readable, writable, append);
    }
};
static std::unique_ptr<Open> opens;

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
    Open* open{};
    io_ticket ticket{};
    read_job read{};
};
static std::array<pending, io_queue_depth> reads;
static auto map_read(const read_job& read) noexcept -> std::expected<io_extent, status_t> {
    auto& node = *reinterpret_cast<inode*>(read.object);
    return volume.extent(node.state, read.offset + read.done, read.size - read.done);
}
// CTZ lookup may itself read metadata. Leave one of the 32 device credits
// available so lookup never waits for a credit held by its own data reader.
// Filesystem mapping stays with its owner. This reader only schedules bounded
// physical reads and copies the authorized bytes from each aligned extent.
// Exact-id completion leaves metadata I/O and other owners' results untouched.
class fs_reader final {
    struct flight {
        read_job* read{};
        uint64_t id{}, buffer{};
        io_extent extent{};
    };
    std::array<flight, io_queue_depth - 1> flights_{};

    static auto cancelled(const read_job& read) noexcept -> bool {
        return read.cancelled && read.cancelled(read);
    }
    static void finish(read_job& read, status_t status) noexcept {
        read.active = read.submitted = false;
        read.complete(read, status);
    }
    void complete(flight& slot, const io_done& result) noexcept {
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
                io_done result{};
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
                io_done result{};
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
        if (const auto status = rt_admit(read.deadline); status != STATUS_OK) {
            finish(read, status);
            return true;
        }
        if (read.done == read.size) {
            finish(read, STATUS_OK);
            return true;
        }
        for (auto& slot : flights_)
            if (!slot.read) {
                const auto io_extent = map_read(read);
                if (!io_extent) {
                    finish(read, io_extent.error());
                    return true;
                }
                if (!io_extent->bytes || io_extent->bytes > read.size - read.done ||
                    io_extent->skip > io_extent->size ||
                    io_extent->bytes > io_extent->size - io_extent->skip)
                    return std::unexpected(STATUS_INTERNAL);
                io_cmd request{.operation = io_operation::Read,
                               .offset = io_extent->offset,
                               .length = io_extent->size,
                               .deadline = read.deadline};
                const auto status = backend.submit(request);
                if (status == STATUS_BUSY)
                    return false;
                if (status != STATUS_OK) {
                    finish(read, status);
                    return true;
                }
                read.submitted = true;
                slot = {&read, request.id, request.buffer_offset, *io_extent};
                return true;
            }
        return false;
    }
};

static fs_reader reader;

static auto cancelled(const read_job& read) noexcept -> bool {
    const auto& pending = *static_cast<const ::pending*>(read.context);
    auto& session = pending.open->session;
    return session.failed() || session.queue()->cancelled(pending.ticket);
}
static void completed(read_job& read, status_t status) noexcept {
    auto& pending = *static_cast<::pending*>(read.context);
    auto& open = *pending.open;
    auto* queue = open.session.queue();
    const auto* request = queue->request(pending.ticket);
    if (request->sequential) {
        if (status == STATUS_OK)
            open.offset += read.done;
        open.busy = false;
    }
    if (open.session.failed() || !queue->finish(pending.ticket, status, read.done)) {
        open.session.abort();
        if (!queue->abandon(pending.ticket))
            sys::exit(STATUS_INTERNAL);
    }
    pending = {};
    if (status != STATUS_OK && status != STATUS_CANCELED && status != STATUS_TIMED_OUT)
        sys::exit(status);
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
        sys::exit(polled.error());
    bool progress = *polled;
    for (auto& pending : reads) {
        const auto submitted = reader.submit(pending.read);
        if (!submitted)
            sys::exit(submitted.error());
        progress |= *submitted;
    }
    return progress;
}
static void drain_reads() noexcept {
    while (reading()) {
        (void) pump_reads();
        if (reading())
            if (const auto rc = reader.wait(); rc != STATUS_OK)
                sys::exit(rc);
    }
}
static auto start_read(Open& open, io_ticket ticket, pending& pending) noexcept -> bool {
    const auto& request = *open.session.queue()->request(ticket);
    auto& file = open.file->state;
    const auto offset = request.sequential ? open.offset : request.offset;
    if (request.operation != io_operation::Read || !open.readable ||
        !Open::valid(request) || offset > LFS_FILE_MAX || request.length > LFS_FILE_MAX - offset ||
        (file.flags & (LFS_F_INLINE | LFS_F_WRITING)))
        return false;
    if (const auto rc = backend.drain_writes(); rc != STATUS_OK)
        sys::exit(rc);
    const auto available = offset < file.ctz.size ? file.ctz.size - offset : 0;
    pending = {.open = &open,
               .ticket = ticket,
               .read = {.object = reinterpret_cast<uintptr_t>(open.file),
                        .offset = offset,
                        .deadline = request.deadline,
                        .output = open.session.payload() + request.buffer_offset,
                        .size = std::min<uint64_t>(request.length, available),
                        .context = &pending,
                        .complete = completed,
                        .cancelled = cancelled,
                        .active = true}};
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
        const auto status = open_file(*source, path, fs_read);
        if (status != STATUS_OK)
            return status;
    }
    const auto size = lfs_file_size(volume.fs(), &source->state);
    if (size <= 0)
        return size < 0 ? volume.error(size) : STATUS_BAD_ARGS;
    const auto bytes = (uint64_t(size) + 4095) & ~uint64_t{4095};
    fs_image result{.size = uint64_t(size)};
    auto status = result.create(store_memory, VM_READ | VM_WRITE | (perms & VM_EXECUTE));
    if (status != STATUS_OK)
        return status;
    vm_area staging;
    status = staging.open(rt_vm(), 0, bytes, VM_READ | VM_WRITE);
    if (status == STATUS_OK)
        status = staging.attach(result.memory.selector(), 0, bytes, VM_READ | VM_WRITE);
    if (status != STATUS_OK)
        return status;
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
        status = sys::memory_seal(result.memory.selector()).status;
    if (status != STATUS_OK)
        return status;
    return result.offer(response, descriptor, perms, store_memory.account);
}

static auto control(std::string_view method, ubus_request& request, ubus_msg& response) -> status_t {
    drain_reads();
    if (const auto status = rt_admit(request.deadline); status != STATUS_OK) return status;
    if (method == "DeviceId") {
        uint8_t id[20]{};
        const auto status = backend.identify(id);
        if (status == STATUS_OK) response.data.put("id", std::as_bytes(std::span{id}));
        return status;
    }
    if (method == "VolumeId") {
        uint8_t id[fs_volume_id_size]{};
        const auto status = volume.identity(id);
        if (status == STATUS_OK) response.data.put("id", std::as_bytes(std::span{id}));
        return status;
    }
    if (method == "Format") {
        const auto id = request.data["id"].as<std::span<const std::byte>>();
        if (id && id->size() != fs_volume_id_size) return STATUS_BAD_ARGS;
        if (require_id && (!id || !expected(reinterpret_cast<const uint8_t*>(id->data())))) return STATUS_DENIED;
        if (opens) return STATUS_BUSY;
        nodes.reset(); ++epoch;
        return volume.format(id ? reinterpret_cast<const uint8_t*>(id->data()) : nullptr);
    }
    if (!volume.mounted()) return STATUS_BACKING_FAILED;
    if (method == "Sync") {
        for (auto* node = nodes.get(); node; node = node->next.get())
            if (const auto status = writeback(*node); status != STATUS_OK) return status;
        return backend.flush();
    }
    const auto raw = request.data[method == "Rename" ? "from" : "path"].as<std::string_view>();
    if (!raw || raw->find(char{}) != std::string_view::npos) return STATUS_BAD_ARGS;
    const auto path = fs_path(*raw);
    if (method == "Map") {
        const auto perms = *request.data["perms"].as<word_t>();
        if ((perms & VM_EXECUTE) && !(request.facet() & fs_execute)) return STATUS_DENIED;
        return snapshot(path.c_str(), perms, response);
    }
    if (method == "Open") {
        const auto flags = *request.data["flags"].as<uint64_t>();
        if (!(flags & (fs_read | fs_write)) || flags & ~(fs_read | fs_write | fs_create | fs_truncate | fs_exclusive | fs_append)
            || ((flags & ~(fs_read | fs_write)) && !(flags & fs_write))) return STATUS_BAD_ARGS;
        if ((flags & fs_write) && !(request.facet() & fs_modify)) return STATUS_DENIED;
        auto opened = std::make_unique<Open>();
        const auto status = opened->create(flags, path.c_str(), response);
        if (status != STATUS_OK) return status;
        if (flags & (fs_create | fs_truncate)) ++epoch;
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
        const auto to = fs_path(target);
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
    const auto& info = rt_startup();
    events = info.events;
    store_memory = {info.selector({"image.pool", ObjAccount}), info.selector({"image.scope", ObjScope})};
    if (!store_memory.account || !store_memory.scope) return STATUS_DENIED;
    if (argc > 2) return STATUS_BAD_ARGS;
    if (argc == 2) {
        if (!fs_parse_volume_id(argv[1], expected_id)) return STATUS_BAD_ARGS;
        require_id = true;
    }
    ubus_conn bus, block;
    auto status = bus.bind(info.selector(ubus_directory));
    if (status == STATUS_OK) status = block.connect(bus, "block", "myos.block", 1, 1);
    if (status == STATUS_OK) status = backend.connect(block);
    if (status == STATUS_OK) status = volume.open(backend);
    if (status != STATUS_OK) return status;
    if (require_id && volume.mounted()) {
        uint8_t actual[fs_volume_id_size]{};
        if (const auto rc = volume.identity(actual); rc != STATUS_OK) return rc;
        if (!expected(actual)) return STATUS_DENIED;
    }
    struct method { std::string_view name; ubus_schema input, output; word_t rights; };
    for (auto& item : std::array<method, 11>{{
        {"Map", {{"path", ubus_type::String}, {"perms", ubus_type::U64}}, {{"buffer", ubus_type::Buffer}}, fs_lookup},
        {"Open", {{"path", ubus_type::String}, {"flags", ubus_type::U64}}, {{"handle", ubus_type::Cap}}, fs_lookup},
        {"List", {{"path", ubus_type::String}, {"cursor", ubus_type::Record, true}}, {{"epoch", ubus_type::U64}, {"items", ubus_type::Array}, {"cursor", ubus_type::Record, true}}, fs_lookup},
        {"Remove", {{"path", ubus_type::String}}, {}, fs_modify},
        {"Mkdir", {{"path", ubus_type::String}}, {}, fs_modify},
        {"Rename", {{"from", ubus_type::String}, {"to", ubus_type::String}}, {}, fs_modify},
        {"Sync", {}, {}, fs_modify},
        {"DeviceId", {}, {{"id", ubus_type::Bytes}}, fs_lookup},
        {"VolumeId", {}, {{"id", ubus_type::Bytes}}, fs_lookup},
        {"Format", {{"id", ubus_type::Bytes, true}}, {}, fs_admin},
        {"Capacity", {}, {{"bytes", ubus_type::U64}}, fs_lookup}
    }}) {
        const auto name = item.name;
        status = directory.add(std::string{name}, std::move(item.input), std::move(item.output),
            [name](auto& request, auto& response) {
                if (name == "Capacity") { response.data.put("bytes", backend.capacity()); return STATUS_OK; }
                const auto status = control(name, request, response);
                if (volume.failed()) sys::exit(volume.error(-1));
                reap(); return status;
            }, item.rights);
        if (status != STATUS_OK) return status;
    }
    if ((status = directory.open()) != STATUS_OK) return status;
    ubus_conn publisher;
    if ((status = publisher.bind(info.selector(ubus_publisher))) != STATUS_OK) return status;
    for (const auto& [name, rights] : std::array<std::pair<std::string_view, word_t>, 4>{{
        {"files", fs_lookup | fs_modify | fs_execute}, {"read", fs_lookup}, {"exec", fs_lookup | fs_execute}, {"admin", fs_admin | fs_lookup | fs_modify}
    }}) if (const auto published = directory.publish(publisher, name, rights); !published) return published.error();
    if (const auto ready = info.selector(ubus_ready)) if ((status = sys::notification_signal(ready).status) != STATUS_OK) return status;
    for (;;) {
        if (const auto status = backend.error(); status != STATUS_OK) sys::exit(status);
        bool again = pump_reads();
        for (size_t count = 0; count < io_queue_depth; ++count) {
            const auto status = directory.poll();
            if (status == STATUS_WOULD_BLOCK) break;
            if (status != STATUS_OK) return status;
            again = true;
        }
        for (auto* opened = opens.get(); opened; opened = opened->next.get())
            if (opened->session.poll() != STATUS_OK) { opened->session.abort(); again = true; }
        for (auto* owner = opens.get(); owner; owner = owner->next.get()) {
            auto& opened = *owner;
            auto* queue = opened.session.queue();
            if (opened.session.closing()) continue;
            for (size_t row = 0; row < io_queue_depth && !opened.busy; ++row) {
                auto* pending = slot();
                if (!pending) break;
                io_ticket ticket{};
                if (queue->admit(ticket) != io_admission::Ready) break;
                auto status = queue->cancelled(ticket) ? STATUS_CANCELED : rt_admit(queue->request(ticket)->deadline);
                if (status != STATUS_OK) {
                    if (!queue->finish(ticket, status, 0)) return STATUS_INTERNAL;
                    again = true; continue;
                }
                if (start_read(opened, ticket, *pending)) { again = true; continue; }
                drain_reads();
                status = queue->cancelled(ticket) ? STATUS_CANCELED : rt_admit(queue->request(ticket)->deadline);
                if (status == STATUS_OK && !queue->commit(ticket)) status = STATUS_CANCELED;
                const auto completion = status == STATUS_OK ? opened.transfer(*queue->request(ticket)) : io_done{ticket.id, status};
                if (!queue->finish(ticket, completion.status, completion.bytes)) return STATUS_INTERNAL;
                again = true;
            }
        }
        again |= pump_reads();
        for (auto* link = &opens; *link;) {
            auto& opened = **link;
            auto status = STATUS_OK;
            if (opened.session.closing() && !opened.session.queue()->active() && opened.file->users == 1) {
                drain_reads();
                auto& file = *opened.file;
                if (file.state.m.pair[0] != UINT32_MAX && file.state.m.pair[1] != UINT32_MAX
                    && (file.state.flags & (LFS_F_DIRTY | LFS_F_WRITING))) status = writeback(file);
            }
            if (const auto rc = opened.session.flush(true, status); rc != STATUS_OK) return rc;
            if (opened.session.done()) {
                again = true;
                auto retired = std::move(*link); *link = std::move(retired->next);
            } else link = &opened.next;
        }
        reap();
        if (const auto rc = directory.arm(); rc != STATUS_OK) return rc;
        if (!again) if (const auto rc = ev_wait(events).status; rc != STATUS_OK) return rc;
    }
}
