#include <util.hpp>
#include <string_view>
#include <string.h>
#include <utility>
#include <ipc.hpp>
#include <servers/read.hpp>
#include <servers/volume.hpp>
#include <start.hpp>
#include <protocols/file.hpp>
#include <protocols/block.hpp>
#include <protocols/service.hpp>
#include <expected>
#include <deque>
#include <memory>
#include <vector>

static ch_port directory;
static blk_client backend;
static st_vol volume;
static cap_t events;
static cap_t store_pool, store_memory, store_cspace, store_vspace;
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
    bool open{};
    ~inode() {
        if (open)
            lfs_file_drop(volume.fs(), &state);
    }
};
static std::vector<std::shared_ptr<inode>> nodes;

static void drain_reads() noexcept;

static void reap() noexcept {
    // The registry retains dirty dormant inodes: littlefs' mlist pins their
    // uncommitted CTZ blocks. Open descriptions share the same inode identity.
    std::erase_if(nodes, [](const auto& node) {
        return node.use_count() == 1 &&
               (!(node->state.flags & (LFS_F_DIRTY | LFS_F_WRITING)) ||
                node->state.m.pair[0] == UINT32_MAX || node->state.m.pair[1] == UINT32_MAX);
    });
}
static auto writeback(inode& node) noexcept -> status_t {
    if (node.state.flags & LFS_F_ERRED)
        return STATUS_BACKING_FAILED;
    const auto status = volume.error(lfs_file_sync(volume.fs(), &node.state));
    if (volume.failed())
        sys::exit(status);
    return status;
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
static auto find(const entry& entry) -> std::shared_ptr<inode> {
    for (auto& node : nodes)
        if (node->state.id == entry.id) {
            const auto& a = node->state.m.pair;
            const auto& b = entry.dir.pair;
            if (a[0] == b[0] || a[0] == b[1] || a[1] == b[0] || a[1] == b[1])
                return node;
        }
    return nullptr;
}

[[nodiscard]] auto open_file(inode& file, const char* path, uint64_t flags) noexcept -> status_t {
    file.config = {};
    file.config.buffer = file.cache;
    const bool write = (flags & st_write) != 0;
    int mode = (flags & (st_read | st_write)) == (st_read | st_write) ? LFS_O_RDWR
               : write                                                ? LFS_O_WRONLY
                                                                      : LFS_O_RDONLY;
    if (flags & st_create)
        mode |= LFS_O_CREAT;
    if (flags & st_truncate)
        mode |= LFS_O_TRUNC;
    if (flags & st_exclusive)
        mode |= LFS_O_EXCL;
    const int result = lfs_file_opencfg(volume.fs(), &file.state, path, mode, &file.config);
    if (result < 0)
        return volume.error(result);
    file.open = true;
    return STATUS_OK;
}

[[nodiscard]] auto transfer(inode& file, const io_cmd& request, uint8_t* data, uint64_t& logical,
                            bool implicit, bool readable, bool writable,
                            bool append = false) noexcept -> io_done {
    const bool write = request.operation == static_cast<uint64_t>(io_operation::Write);
    if (!write && request.operation != static_cast<uint64_t>(io_operation::Read))
        return {request.id, STATUS_INVALID_OP, 0, 0};
    if ((write && !writable) || (!write && !readable))
        return {request.id, STATUS_DENIED, 0, 0};
    auto offset = implicit ? logical : request.offset;
    if (write && implicit && append) {
        const auto size = lfs_file_size(volume.fs(), &file.state);
        if (size < 0)
            return {request.id, volume.error(size), 0, 0};
        offset = size;
    }
    if (offset > LFS_FILE_MAX || request.length > LFS_FILE_MAX - offset)
        return {request.id, STATUS_BAD_ARGS, 0, 0};
    const auto seek = lfs_file_seek(volume.fs(), &file.state, offset, LFS_SEEK_SET);
    if (seek < 0) {
        const auto status = volume.error(seek);
        if (volume.failed())
            sys::exit(status);
        return {request.id, status, 0, 0};
    }
    const auto result = write ? lfs_file_write(volume.fs(), &file.state, data, request.length)
                              : lfs_file_read(volume.fs(), &file.state, data, request.length);
    if (volume.failed())
        sys::exit(volume.error(result));
    if (result > 0 && implicit)
        logical = offset + result;
    return {request.id, volume.error(result < 0 ? result : 0),
            result > 0 ? static_cast<uint64_t>(result) : 0, 0};
}

struct Open final {
    std::shared_ptr<inode> file;
    cap_owner channel;
    io_server session;
    uint64_t offset{};
    bool busy{};
    bool readable{}, writable{}, append{};

    [[nodiscard]] auto create(const ch_msg& request, const char* path, ch_reply& reply) noexcept
        -> status_t {
        const auto pair = sys::channel_create(store_pool, 1, CHANNEL_MAX_WORDS, 4, 2);
        if (pair.status != STATUS_OK)
            return pair.status;
        cap_owner client_root{pair.value}, server_root{pair.value2};
        constexpr auto common = RIGHT_SEND | RIGHT_RECEIVE | RIGHT_CLOSE;
        const auto client = sys::channel_mint(client_root.selector(), store_cspace, 1,
                                              common | RIGHT_DESTROY | RIGHT_DUPLICATE);
        const auto server = sys::channel_mint(server_root.selector(), store_cspace, 1, common);
        cap_owner endpoint{client.value};
        channel = cap_owner{server.value};
        if (client.status != STATUS_OK || server.status != STATUS_OK)
            return client.status != STATUS_OK ? client.status : server.status;
        auto status = session.open(channel.selector(), events, store_memory, store_vspace,
                                   store_cspace, 0, 0, true);
        if (status == STATUS_OK) {
            const auto entry = lookup(path);
            if (!entry && entry.error() != LFS_ERR_NOENT)
                status = volume.error(entry.error());
            if (entry)
                file = find(*entry);
            if (file) {
                if ((request.value & st_exclusive) && (request.value & st_create))
                    status = file_exists;
                else if (request.value & st_truncate) {
                    const auto result = lfs_file_truncate(volume.fs(), &file->state, 0);
                    status = volume.error(result);
                }
            } else if (status == STATUS_OK) {
                file.reset(new inode);
                const auto flags = (request.value & ~(st_read | st_write)) | st_read | st_write;
                status = open_file(*file, path, flags);
                if (status == STATUS_OK)
                    nodes.push_back(file);
            }
        }
        if (status != STATUS_OK)
            return status;
        if (!reply.offer(std::move(endpoint), common | RIGHT_DESTROY | RIGHT_DUPLICATE))
            return STATUS_INTERNAL;
        readable = (request.value & st_read) != 0;
        writable = (request.value & st_write) != 0;
        append = (request.value & st_append) != 0;
        return STATUS_OK;
    }

    void control(const ch_msg& request, ch_reply& reply) noexcept {
        drain_reads();
        if (request.operation == static_cast<uint64_t>(io_control::Stat) && request.size == 0 &&
            request.value == 0) {
            const auto size = lfs_file_size(volume.fs(), &file->state);
            reply.message.status = volume.error(size < 0 ? size : 0);
            if (size >= 0)
                reply.message.value = size;
            return;
        }
        if (request.operation != static_cast<uint64_t>(io_control::Sync) || request.size != 0 ||
            request.value != 0) {
            reply.message.status = STATUS_INVALID_OP;
            return;
        }
        reply.message.status = writeback(*file);
        if (reply.message.status == STATUS_OK)
            reply.message.status = backend.flush();
        if (reply.message.status == STATUS_BACKING_FAILED)
            sys::exit(reply.message.status);
    }

    static auto valid(const io_cmd& request) noexcept -> bool {
        return !request.object && !request.buffer && request.flags <= 1 && request.length &&
               request.length <= io_buffer_size && request.buffer_offset <= io_payload_size &&
               request.length <= io_payload_size - request.buffer_offset;
    }
    [[nodiscard]] auto transfer(const io_cmd& request) noexcept -> io_done {
        if (!valid(request))
            return {request.id, STATUS_BAD_ARGS, 0, 0};
        return ::transfer(*file, request, session.payload() + request.buffer_offset, offset,
                          request.flags == 1, readable, writable, append);
    }
};
static std::deque<std::unique_ptr<Open>> opens;

// The ticket retains its endpoint and inode until the device stops accessing
// the payload. Filesystem mutation is serialized behind these physical reads.
struct pending {
    Open* open{};
    io_ticket ticket{};
    io_read read{};
};
static std::array<pending, io_queue_depth> reads;
static auto map_read(const io_read& read) noexcept -> std::expected<io_extent, status_t> {
    auto& node = *reinterpret_cast<inode*>(read.object);
    return volume.extent(node.state, read.offset + read.done, read.size - read.done);
}
// CTZ lookup may itself read metadata. Leave one of the 32 device credits
// available so lookup never waits for a credit held by its own data reader.
static io_reader<blk_client, decltype(&map_read), io_queue_depth - 1> reader{backend, map_read};

static auto cancelled(const io_read& read) noexcept -> bool {
    const auto& pending = *static_cast<const ::pending*>(read.context);
    auto& session = pending.open->session;
    return session.failed() || session.queue()->cancelled(pending.ticket);
}
static void completed(io_read& read, status_t status) noexcept {
    auto& pending = *static_cast<::pending*>(read.context);
    auto& open = *pending.open;
    auto* queue = open.session.queue();
    const auto* request = queue->request(pending.ticket);
    if (request->flags == 1) {
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
    if (status != STATUS_OK && status != STATUS_CANCELED)
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
    const auto offset = request.flags == 1 ? open.offset : request.offset;
    if (request.operation != static_cast<uint64_t>(io_operation::Read) || !open.readable ||
        !Open::valid(request) || offset > LFS_FILE_MAX || request.length > LFS_FILE_MAX - offset ||
        (file.flags & (LFS_F_INLINE | LFS_F_WRITING)))
        return false;
    if (const auto rc = backend.drain_writes(); rc != STATUS_OK)
        sys::exit(rc);
    const auto available = offset < file.ctz.size ? file.ctz.size - offset : 0;
    pending = {.open = &open,
               .ticket = ticket,
               .read = {.object = reinterpret_cast<uintptr_t>(open.file.get()),
                        .offset = offset,
                        .output = open.session.payload() + request.buffer_offset,
                        .size = std::min<uint64_t>(request.length, available),
                        .context = &pending,
                        .complete = completed,
                        .cancelled = cancelled,
                        .active = true}};
    if (request.flags == 1)
        open.busy = true;
    return true;
}

static auto path_from(const ch_msg& request, char (&out)[sizeof(ch_msg::data) + 1]) noexcept
    -> bool {
    if (request.size == 0 || request.size > sizeof(request.data))
        return false;
    ::memcpy(out, request.data, request.size);
    out[request.size] = '\0';
    for (size_t i = 0; i < request.size; ++i)
        if (out[i] == '\0' && request.operation != static_cast<uint64_t>(st_control::Rename))
            return false;
    return true;
}

static void control(const ch_msg& request, ch_reply& response, bool writable, bool admin) noexcept {
    drain_reads();
    auto& reply = response.message;
    const auto operation = static_cast<st_control>(request.operation);
    if (operation == st_control::DeviceId) {
        if (request.size != 0 || request.value != 0) {
            reply.status = STATUS_BAD_ARGS;
            return;
        }
        uint8_t id[20]{};
        reply.status = backend.identify(id);
        if (reply.status == STATUS_BACKING_FAILED)
            sys::exit(reply.status);
        if (reply.status == STATUS_OK) {
            reply.size = sizeof(id);
            ::memcpy(reply.data, id, sizeof(id));
        }
        return;
    }
    if (operation == st_control::VolumeId) {
        if (request.size != 0 || request.value != 0) {
            reply.status = STATUS_BAD_ARGS;
            return;
        }
        uint8_t id[st_volume_id_size]{};
        reply.status = volume.identity(id);
        if (volume.failed())
            sys::exit(volume.error(-1));
        if (reply.status == STATUS_OK) {
            reply.size = sizeof(id);
            ::memcpy(reply.data, id, sizeof(id));
        }
        return;
    }
    if (operation == st_control::Format) {
        if (!admin) {
            reply.status = STATUS_DENIED;
            return;
        }
        if ((request.size != 0 && request.size != st_volume_id_size) || request.value != 0) {
            reply.status = STATUS_BAD_ARGS;
            return;
        }
        if (require_id && (request.size != st_volume_id_size ||
                           !expected(reinterpret_cast<const uint8_t*>(request.data)))) {
            reply.status = STATUS_DENIED;
            return;
        }
        if (!opens.empty()) {
            reply.status = STATUS_BUSY;
            return;
        }
        nodes.clear();
        reply.status = volume.format(
            request.size == 0 ? nullptr : reinterpret_cast<const uint8_t*>(request.data));
        if (volume.failed())
            sys::exit(volume.error(-1));
        return;
    }
    if (!volume.mounted()) {
        reply.status = STATUS_BACKING_FAILED;
        return;
    }
    if (operation == st_control::Sync) {
        if (!writable) {
            reply.status = STATUS_DENIED;
            return;
        }
        if (request.size || request.value) {
            reply.status = STATUS_BAD_ARGS;
            return;
        }
        for (auto& node : nodes) {
            reply.status = writeback(*node);
            if (reply.status != STATUS_OK)
                return;
        }
        reply.status = backend.flush();
        if (reply.status != STATUS_OK)
            sys::exit(reply.status);
        reap();
        return;
    }
    if (operation == st_control::Open) {
        char path[sizeof(request.data) + 1]{};
        if (!path_from(request, path) || request.value == 0 ||
            (request.value &
             ~(st_read | st_write | st_create | st_truncate | st_exclusive | st_append)) != 0 ||
            ((request.value & (st_create | st_truncate | st_exclusive | st_append)) != 0 &&
             (request.value & st_write) == 0)) {
            reply.status = STATUS_BAD_ARGS;
            return;
        }
        const bool write = (request.value & st_write) != 0;
        if (write && !writable) {
            reply.status = STATUS_DENIED;
            return;
        }
        auto opened = std::make_unique<Open>();
        reply.status = opened->create(request, path, response);
        if (reply.status == STATUS_OK)
            opens.push_back(std::move(opened));
    } else if (operation == st_control::List) {
        char path[sizeof(request.data) + 1]{};
        if (request.value > UINT32_MAX) {
            reply.status = STATUS_BAD_ARGS;
            return;
        }
        if (request.size == 0)
            path[0] = '/';
        else if (!path_from(request, path)) {
            reply.status = STATUS_BAD_ARGS;
            return;
        }
        lfs_dir_t cursor{};
        int result = lfs_dir_open(volume.fs(), &cursor, path);
        const bool opened = result == 0;
        if (result == 0 && request.value != 0)
            result = lfs_dir_seek(volume.fs(), &cursor, request.value);
        lfs_info entry{};
        while (result == 0) {
            const auto position = lfs_dir_tell(volume.fs(), &cursor);
            if (position < 0) {
                result = position;
                break;
            }
            result = lfs_dir_read(volume.fs(), &cursor, &entry);
            if (result <= 0) {
                reply.value = 0;
                break;
            }
            if ((std::string_view{entry.name} == ".") || (std::string_view{entry.name} == "..")) {
                result = 0;
                continue;
            }
            const size_t length = ::strlen(entry.name);
            if (length > sizeof(reply.data)) {
                result = LFS_ERR_NAMETOOLONG;
                break;
            }
            const size_t separator = reply.size != 0;
            if (length + separator > sizeof(reply.data) - reply.size) {
                reply.value = position;
                result = 0;
                break;
            }
            if (separator)
                reply.data[reply.size++] = '\0';
            ::memcpy(reply.data + reply.size, entry.name, length);
            reply.size += length;
            const auto next = lfs_dir_tell(volume.fs(), &cursor);
            if (next < 0) {
                result = next;
                break;
            }
            reply.value = next;
            result = 0;
        }
        const int closed = opened ? lfs_dir_close(volume.fs(), &cursor) : 0;
        reply.status = volume.error(result < 0 ? result : closed);
    } else if (operation == st_control::Remove || operation == st_control::Mkdir ||
               operation == st_control::Rename) {
        if (!writable) {
            reply.status = STATUS_DENIED;
            return;
        }
        char path[sizeof(request.data) + 1]{};
        if (!path_from(request, path)) {
            reply.status = STATUS_BAD_ARGS;
            return;
        }
        int result{};
        if (operation == st_control::Remove)
            result = lfs_remove(volume.fs(), path);
        else if (operation == st_control::Mkdir)
            result = lfs_mkdir(volume.fs(), path);
        else {
            // Rename uses a second NUL-terminated path in the same
            // immutable control frame, after the first one.
            const size_t first = ::strlen(path);
            if (first == 0 || first + 1 >= request.size || request.data[first] != '\0') {
                reply.status = STATUS_BAD_ARGS;
                return;
            }
            for (size_t i = first + 1; i < request.size; ++i)
                if (request.data[i] == '\0') {
                    reply.status = STATUS_BAD_ARGS;
                    return;
                }
            const char* target = path + first + 1;
            const auto source = lookup(path);
            auto moved = source ? find(*source) : nullptr;
            if (!source && source.error() != LFS_ERR_ISDIR)
                result = source.error();
            else {
                result = lfs_rename(volume.fs(), path, target);
                if (result == 0 && moved) {
                    const auto entry = lookup(target);
                    if (!entry)
                        result = entry.error();
                    else {
                        // littlefs detaches deleted entries, including the
                        // source of rename. Keep this inode at the new link;
                        // the replaced inode remains detached.
                        moved->state.m = entry->dir;
                        moved->state.id = entry->id;
                    }
                }
            }
        }
        reply.status = volume.error(result);
    } else
        reply.status = STATUS_INVALID_OP;
    if (volume.failed())
        sys::exit(volume.error(-1));
    reap();
}

int main(int argc, char** argv) {

    const auto& info = rt_startup();
    const auto pool = info.pool;
    const auto vspace = info.vspace;
    const auto cspace = info.cspace;
    store_pool = pool;
    store_memory = info.selector(io_memory);
    if (!store_memory) sys::exit(STATUS_DENIED);
    store_cspace = cspace;
    store_vspace = vspace;
    events = info.events;
    if (argc > 2)
        sys::exit(STATUS_BAD_ARGS);
    if (argc == 2) {
        const auto* text = argv[1];
        if (!st_parse_volume_id(text, ::strlen(text), expected_id))
            sys::exit(STATUS_BAD_ARGS);
        require_id = true;
    }
    if (const auto rc =
            backend.connect(info.selector(blk_service), pool, cspace, events, vspace, 0);
        rc != STATUS_OK)
        sys::exit(rc);
    if (const auto rc = volume.open(backend); rc != STATUS_OK)
        sys::exit(rc);
    if (require_id && volume.mounted()) {
        uint8_t actual[st_volume_id_size]{};
        if (const auto rc = volume.identity(actual); rc != STATUS_OK)
            sys::exit(rc);
        if (!expected(actual))
            sys::exit(STATUS_DENIED);
    }
    if (const auto rc = directory.open(info.selector(st_service), events); rc != STATUS_OK)
        sys::exit(rc);
    if (const auto rc = sys::notification_signal(info.selector(sv_ready)).status; rc != STATUS_OK)
        sys::exit(rc);
    if (const auto rc = sys::notification_signal(info.selector(sv_wake)).status; rc != STATUS_OK)
        sys::exit(rc);
    for (;;) {
        bool again = pump_reads();
        // Bound admission per pass so file completions continue under load.
        for (size_t count = 0; count < io_queue_depth; ++count) {
            ch_packet packet;
            const auto status = directory.receive(packet);
            if (status == STATUS_WOULD_BLOCK)
                break;
            if (status == STATUS_BAD_ARGS || status == STATUS_TRANSFER_FAILED)
                continue;
            if (status != STATUS_OK)
                sys::exit(status);
            if (packet.count != 1)
                continue;
            again = true;
            ch_reply reply;
            reply.message = {.operation = packet.message.operation, .id = packet.message.id};
            if (packet.badge != st_read_directory && packet.badge != st_write_directory &&
                packet.badge != st_admin_directory)
                reply.message.status = STATUS_DENIED;
            else
                control(packet.message, reply, packet.badge != st_read_directory,
                        packet.badge == st_admin_directory);
            reply.respond(packet.capabilities[0].selector());
        }
        for (auto& owner : opens) {
            auto& opened = *owner;
            if (opened.session.poll([&](const ch_msg& request, ch_reply& reply) {
                    again = true;
                    opened.control(request, reply);
                }) != STATUS_OK) {
                opened.session.abort();
                again = true;
            }
        }
        for (auto& owner : opens) {
            auto& opened = *owner;
            auto* queue = opened.session.queue();
            if (!queue || opened.session.closing())
                continue;
            for (size_t row = 0; row < io_queue_depth && !opened.busy; ++row) {
                auto* pending = slot();
                if (!pending)
                    break;
                io_ticket ticket{};
                const auto admission = queue->admit(ticket);
                if (admission == io_admission::Empty || admission == io_admission::Backpressure)
                    break;
                if (admission != io_admission::Ready) {
                    opened.session.abort();
                    again = true;
                    break;
                }
                if (start_read(opened, ticket, *pending)) {
                    again = true;
                    continue;
                }
                // Native inline/dirty-cache I/O and mutations retain littlefs'
                // cursor/cache ordering and cannot recycle a pinned data block.
                drain_reads();
                const auto completion = opened.transfer(*queue->request(ticket));
                // Nested Block waits share this notification and may consume
                // another session's hint. Drain all queues again before sleep.
                again = true;
                if (!queue->finish(ticket, completion.status, completion.bytes)) {
                    opened.session.abort();
                    if (!queue->abandon(ticket))
                        sys::exit(STATUS_INTERNAL);
                    again = true;
                    break;
                }
            }
        }
        again |= pump_reads();
        for (auto& owner : opens) {
            auto& opened = *owner;
            if (opened.session.flush(true) != STATUS_OK) {
                opened.session.abort();
                again = true;
            }
            if (opened.session.done()) {
                again = true;
                (void) sys::channel_close(opened.channel.selector());
                opened.session.reset();
                opened.channel = {};
            } else if (opened.session.arm() != STATUS_OK) {
                opened.session.abort();
                again = true;
            }
        }
        std::erase_if(opens, [](const auto& opened) { return !opened->channel; });
        reap();
        if (const auto rc = directory.arm().status; rc != STATUS_OK)
            sys::exit(rc);
        if (!again)
            if (const auto rc = sys::notification_wait(events).status; rc != STATUS_OK)
                sys::exit(rc);
    }
}
