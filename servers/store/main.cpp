#include <utility>
#include <servers/runtime/service.hpp>
#include <servers/runtime/queue.hpp>
#include <servers/store/volume.hpp>
#include <sys/start.hpp>
#include <sys/storage.hpp>
#include <expected>

namespace {
using namespace sys;
constexpr size_t Clients = 4;
constexpr size_t Opens = 8;
io::ControlPort directory;
block::client backend;
store::Volume volume;
cap_t events;
cap_t store_pool, store_cspace, store_vspace;
uint8_t expected_id[store::VolumeIdSize]{};
bool require_id{};

auto expected(const uint8_t* id) noexcept -> bool {
    for (size_t i = 0; i < sizeof(expected_id); ++i)
        if (id[i] != expected_id[i]) return false;
    return true;
}

struct inode final {
    lfs_file_t state{};
    lfs_file_config config{};
    uint8_t cache[store::Volume::BlockSize]{};
    bool open{};
    size_t refs{};
};
inode nodes[Opens];

void drain_reads() noexcept;

void drop(inode& node) noexcept {
    lfs_file_drop(volume.fs(), &node.state);
    node.open = false;
}
void reap() noexcept {
    for (auto& node : nodes) if (node.open && !node.refs
        && (!(node.state.flags & (LFS_F_DIRTY | LFS_F_WRITING))
            || node.state.m.pair[0] == UINT32_MAX || node.state.m.pair[1] == UINT32_MAX)) drop(node);
}
auto writeback(inode& node) noexcept -> status_t {
    if (node.state.flags & LFS_F_ERRED) return STATUS_BACKING_FAILED;
    const auto status = volume.error(lfs_file_sync(volume.fs(), &node.state));
    if (volume.failed()) exit(status);
    return status;
}
auto allocate() noexcept -> std::expected<inode*, status_t> {
    for (auto& node : nodes) if (!node.open) return &node;
    // Cache pressure, rather than close, chooses when a dormant inode writes
    // back. Its mlist reference pins uncommitted CTZ blocks until then.
    for (auto& node : nodes) if (!node.refs) {
        const auto status = writeback(node);
        if (status != STATUS_OK) return std::unexpected(status);
        drop(node);
        return &node;
    }
    return std::unexpected(STATUS_NO_MEMORY);
}


// littlefs updates these references on metadata relocation and deletion.
// Resolve namespace identity, never a second pathname/size cache.
struct entry {
    lfs_mdir_t dir;
    uint16_t id;
};
auto lookup(const char* path) -> std::expected<entry, int> {
    uint8_t cache[store::Volume::BlockSize];
    lfs_file_config config{};
    config.buffer = cache;
    lfs_file_t probe{};
    const int opened = lfs_file_opencfg(volume.fs(), &probe, path, LFS_O_RDONLY, &config);
    if (opened < 0) return std::unexpected(opened);
    const entry found{probe.m, probe.id};
    lfs_file_drop(volume.fs(), &probe);
    return found;
}
auto find(const entry& entry) -> inode* {
    for (auto& node : nodes) if (node.open && node.state.id == entry.id) {
        const auto& a = node.state.m.pair;
        const auto& b = entry.dir.pair;
        if (a[0] == b[0] || a[0] == b[1] || a[1] == b[0] || a[1] == b[1]) return &node;
    }
    return nullptr;
}

[[nodiscard]] auto open_file(inode& file, const char* path,
    uint64_t flags, uint64_t& length) noexcept -> status_t {
    file.config = {};
    file.config.buffer = file.cache;
    const bool write = (flags & store::Write) != 0;
    int mode = (flags & (store::Read | store::Write)) == (store::Read | store::Write)
        ? LFS_O_RDWR : write ? LFS_O_WRONLY : LFS_O_RDONLY;
    if (flags & store::Create) mode |= LFS_O_CREAT;
    if (flags & store::Truncate) mode |= LFS_O_TRUNC;
    if (flags & store::Exclusive) mode |= LFS_O_EXCL;
    const int result = lfs_file_opencfg(volume.fs(), &file.state, path, mode, &file.config);
    if (result < 0) return volume.error(result);
    const auto bytes = lfs_file_size(volume.fs(), &file.state);
    if (bytes < 0) {
        lfs_file_drop(volume.fs(), &file.state);
        return volume.error(bytes);
    }
    file.open = true;
    length = bytes;
    return STATUS_OK;
}

[[nodiscard]] auto transfer(inode& file, const io::Request& request,
    uint8_t* data, uint64_t& logical, bool implicit,
    bool readable, bool writable, bool append = false) noexcept -> io::Completion {
    const bool write = request.operation == static_cast<uint64_t>(io::Operation::Write);
    if (!write && request.operation != static_cast<uint64_t>(io::Operation::Read))
        return {request.id, STATUS_INVALID_OP, 0, 0};
    if ((write && !writable) || (!write && !readable))
        return {request.id, STATUS_DENIED, 0, 0};
    auto offset = implicit ? logical : request.offset;
    if (write && implicit && append) {
        const auto size = lfs_file_size(volume.fs(), &file.state);
        if (size < 0) return {request.id, volume.error(size), 0, 0};
        offset = size;
    }
    if (offset > LFS_FILE_MAX || request.length > LFS_FILE_MAX - offset)
        return {request.id, STATUS_BAD_ARGS, 0, 0};
    const auto seek = lfs_file_seek(volume.fs(), &file.state, offset, LFS_SEEK_SET);
    if (seek < 0) {
        const auto status = volume.error(seek);
        if (volume.failed()) exit(status);
        return {request.id, status, 0, 0};
    }
    const auto result = write
        ? lfs_file_write(volume.fs(), &file.state, data, request.length)
        : lfs_file_read(volume.fs(), &file.state, data, request.length);
    if (volume.failed()) exit(volume.error(result));
    if (result > 0 && implicit) logical = offset + result;
    return {request.id, volume.error(result < 0 ? result : 0),
        result > 0 ? static_cast<uint64_t>(result) : 0, 0};
}

struct Open final {
    inode* file{};
    cap::OwnedCap channel;
    io::ServerSession session;
    uint64_t offset{}, generation{};
    bool live{}, busy{};
    bool prepared{};
    bool readable{}, writable{}, append{};

    [[nodiscard]] auto create(const io::ControlMessage& request, const char* path,
        size_t slot, io::ControlReply& reply) noexcept -> status_t {
        if (generation == UINT64_MAX) return STATUS_NO_MEMORY;
        const auto pair = channel_create(store_pool, 1, CHANNEL_MAX_WORDS, 4, 2);
        if (pair.status != STATUS_OK) return pair.status;
        cap::OwnedCap client_root{{pair.value, 0}}, server_root{{pair.value2, 0}};
        constexpr auto common = RIGHT_SEND | RIGHT_RECEIVE | RIGHT_CLOSE;
        const auto client = channel_mint(client_root.selector(), store_cspace, 1,
            common | RIGHT_DESTROY | RIGHT_DUPLICATE);
        const auto server = channel_mint(server_root.selector(), store_cspace, 1, common);
        if (client.status != STATUS_OK || server.status != STATUS_OK) {
            (void)object_destroy(client_root.selector());
            return client.status != STATUS_OK ? client.status : server.status;
        }
        cap::OwnedCap endpoint{{client.value, 0}};
        channel = cap::OwnedCap{{server.value, 0}};
        auto status = STATUS_OK;
        if (!prepared) {
            status = session.prepare(store_pool, store_vspace, store_cspace,
                0x72000000 + slot * 0x100000, true);
            if (status == STATUS_OK) prepared = true;
        }
        if (status == STATUS_OK) status = session.bind(channel.selector(), events, generation + 1);
        uint64_t length{};
        if (status == STATUS_OK) {
            const auto entry = lookup(path);
            if (!entry && entry.error() != LFS_ERR_NOENT) status = volume.error(entry.error());
            if (entry) file = find(*entry);
            if (file) {
                if ((request.value & store::Exclusive) && (request.value & store::Create))
                    status = STATUS_BUSY;
                else if (request.value & store::Truncate) {
                    const auto result = lfs_file_truncate(volume.fs(), &file->state, 0);
                    status = volume.error(result);
                }
                if (status == STATUS_OK) {
                    const auto bytes = lfs_file_size(volume.fs(), &file->state);
                    status = volume.error(bytes < 0 ? bytes : 0);
                    if (status == STATUS_OK) length = bytes;
                }
            } else if (status == STATUS_OK) {
                const auto available = allocate();
                if (!available) status = available.error();
                else {
                    file = *available;
                    const auto flags = (request.value & ~(store::Read | store::Write))
                        | store::Read | store::Write;
                    status = open_file(*file, path, flags, length);
                }
            }
        }
        if (status != STATUS_OK) {
            reap();
            file = nullptr;
            (void)object_destroy(endpoint.selector());
            channel = {};
            session.abort();
            if (session.done()) session.reset();
            return status;
        }
        if (!reply.offer(std::move(endpoint), common | RIGHT_DESTROY
            | RIGHT_DUPLICATE)) {
            reap();
            file = nullptr;
            (void)object_destroy(endpoint.selector());
            channel = {};
            session.abort();
            if (session.done()) session.reset();
            return STATUS_INTERNAL;
        }
        live = true;
        ++file->refs;
        readable = (request.value & store::Read) != 0;
        writable = (request.value & store::Write) != 0;
        append = (request.value & store::Append) != 0;
        offset = request.value & store::Append ? length : 0;
        reply.message.value = ++generation;
        reply.message.size = sizeof(length);
        service::copy(reply.message.data, &length, sizeof(length));
        return STATUS_OK;
    }

    void control(const io::ControlMessage& request, io::ControlReply& reply) noexcept {
        drain_reads();
        if (request.operation == static_cast<uint64_t>(io::Control::Stat)
            && request.size == 0 && request.value == 0) {
            const auto size = lfs_file_size(volume.fs(), &file->state);
            reply.message.status = volume.error(size < 0 ? size : 0);
            if (size >= 0) reply.message.value = size;
            return;
        }
        if (request.operation != static_cast<uint64_t>(io::Control::Sync)
            || request.size != 0 || request.value != 0) {
            reply.message.status = STATUS_INVALID_OP;
            return;
        }
        reply.message.status = writeback(*file);
        if (reply.message.status == STATUS_OK) reply.message.status = backend.flush();
        if (reply.message.status == STATUS_BACKING_FAILED) exit(reply.message.status);
    }

    static auto valid(const io::Request& request) noexcept -> bool {
        return !request.object && !request.buffer && request.flags <= 1
            && request.length && request.length <= io::BufferSize
            && request.buffer_offset <= io::PayloadSize
            && request.length <= io::PayloadSize - request.buffer_offset;
    }
    [[nodiscard]] auto transfer(const io::Request& request) noexcept -> io::Completion {
        if (!valid(request)) return {request.id, STATUS_BAD_ARGS, 0, 0};
        return ::transfer(*file, request, session.payload() + request.buffer_offset,
            offset, request.flags == 1, readable, writable, append);
    }
};
Open opens[Opens];

// The ticket retains its endpoint and inode until the device stops accessing
// the payload. Filesystem mutation is serialized behind these physical reads.
struct pending {
    Open* open{};
    io::Ticket ticket{};
    io::read read{};
};
std::array<pending, io::QueueDepth> reads;
auto map_read(const io::read& read) noexcept -> std::expected<io::extent, status_t> {
    auto& node = *reinterpret_cast<inode*>(read.object);
    return volume.extent(node.state, read.offset + read.done, read.size - read.done);
}
// CTZ lookup may itself read metadata. Leave one of the 32 device credits
// available so lookup never waits for a credit held by its own data reader.
io::reader<block::client, decltype(&map_read), io::QueueDepth - 1> reader{backend, map_read};

auto cancelled(const io::read& read) noexcept -> bool {
    const auto& pending = *static_cast<const ::pending*>(read.context);
    auto& session = pending.open->session;
    return session.failed() || session.queue()->cancelled(pending.ticket);
}
void completed(io::read& read, status_t status) noexcept {
    auto& pending = *static_cast<::pending*>(read.context);
    auto& open = *pending.open;
    auto* queue = open.session.queue();
    const auto* request = queue->request(pending.ticket);
    if (request->flags == 1) {
        if (status == STATUS_OK) open.offset += read.done;
        open.busy = false;
    }
    if (open.session.failed() || !queue->finish(pending.ticket, status, read.done)) {
        open.session.abort();
        if (!queue->abandon(pending.ticket)) exit(STATUS_INTERNAL);
    }
    pending = {};
    if (status != STATUS_OK && status != STATUS_CANCELED) exit(status);
}
auto slot() noexcept -> pending* {
    for (auto& pending : reads) if (!pending.open) return &pending;
    return nullptr;
}
auto reading() noexcept -> bool {
    for (const auto& pending : reads) if (pending.open) return true;
    return false;
}
auto pump_reads() noexcept -> bool {
    const auto polled = reader.poll();
    if (!polled) exit(polled.error());
    bool progress = *polled;
    for (auto& pending : reads) {
        const auto submitted = reader.submit(pending.read);
        if (!submitted) exit(submitted.error());
        progress |= *submitted;
    }
    return progress;
}
void drain_reads() noexcept {
    while (reading()) {
        (void)pump_reads();
        if (reading()) service::require(reader.wait());
    }
}
auto start_read(Open& open, io::Ticket ticket, pending& pending) noexcept -> bool {
    const auto& request = *open.session.queue()->request(ticket);
    auto& file = open.file->state;
    const auto offset = request.flags == 1 ? open.offset : request.offset;
    if (request.operation != static_cast<uint64_t>(io::Operation::Read)
        || !open.readable || !Open::valid(request)
        || offset > LFS_FILE_MAX || request.length > LFS_FILE_MAX - offset
        || (file.flags & (LFS_F_INLINE | LFS_F_WRITING))) return false;
    service::require(backend.drain_writes());
    const auto available = offset < file.ctz.size ? file.ctz.size - offset : 0;
    pending = {.open = &open, .ticket = ticket,
        .read = {.object = reinterpret_cast<uintptr_t>(open.file), .offset = offset,
            .output = open.session.payload() + request.buffer_offset,
            .size = std::min<uint64_t>(request.length, available), .context = &pending,
            .complete = completed, .cancelled = cancelled, .active = true}};
    if (request.flags == 1) open.busy = true;
    return true;
}

struct Client final {
    cap::OwnedCap channel;
    io::ServerSession session;
    bool writable{};
    bool admin{};

    void control(const io::ControlMessage& request, io::ControlReply& response) noexcept {
        drain_reads();
        auto& reply = response.message;
        const auto operation = static_cast<store::Control>(request.operation);
        if (operation == store::Control::DeviceId) {
            if (request.size != 0 || request.value != 0) {
                reply.status = STATUS_BAD_ARGS; return;
            }
            uint8_t id[20]{};
            reply.status = backend.identify(id);
            if (reply.status == STATUS_BACKING_FAILED) exit(reply.status);
            if (reply.status == STATUS_OK) {
                reply.size = sizeof(id);
                service::copy(reply.data, id, sizeof(id));
            }
            return;
        }
        if (operation == store::Control::VolumeId) {
            if (request.size != 0 || request.value != 0) {
                reply.status = STATUS_BAD_ARGS; return;
            }
            uint8_t id[store::VolumeIdSize]{};
            reply.status = volume.identity(id);
            if (volume.failed()) exit(volume.error(-1));
            if (reply.status == STATUS_OK) {
                reply.size = sizeof(id);
                service::copy(reply.data, id, sizeof(id));
            }
            return;
        }
        if (operation == store::Control::Format) {
            if (!admin) { reply.status = STATUS_DENIED; return; }
            if ((request.size != 0 && request.size != store::VolumeIdSize) || request.value != 0) {
                reply.status = STATUS_BAD_ARGS; return;
            }
            if (require_id && (request.size != store::VolumeIdSize
                || !expected(reinterpret_cast<const uint8_t*>(request.data)))) {
                reply.status = STATUS_DENIED; return;
            }
            for (const auto& opened : opens)
                if (opened.live) { reply.status = STATUS_BUSY; return; }
            for (auto& node : nodes) if (node.open) drop(node);
            reply.status = volume.format(request.size == 0
                ? nullptr : reinterpret_cast<const uint8_t*>(request.data));
            if (volume.failed()) exit(volume.error(-1));
            return;
        }
        if (!volume.mounted()) { reply.status = STATUS_BACKING_FAILED; return; }
        if (operation == store::Control::Sync) {
            if (!writable) { reply.status = STATUS_DENIED; return; }
            if (request.size || request.value) { reply.status = STATUS_BAD_ARGS; return; }
            for (auto& node : nodes) if (node.open) {
                reply.status = writeback(node);
                if (reply.status != STATUS_OK) return;
            }
            reply.status = backend.flush();
            if (reply.status != STATUS_OK) exit(reply.status);
            reap();
            return;
        }
        if (operation == store::Control::Open) {
            char path[sizeof(request.data) + 1]{};
            if (!path_from(request, path) || request.value == 0
                || (request.value & ~(store::Read | store::Write | store::Create
                    | store::Truncate | store::Exclusive | store::Append)) != 0
                || ((request.value & (store::Create | store::Truncate | store::Exclusive | store::Append)) != 0
                    && (request.value & store::Write) == 0)) {
                reply.status = STATUS_BAD_ARGS;
                return;
            }
            const bool write = (request.value & store::Write) != 0;
            if (write && !writable) { reply.status = STATUS_DENIED; return; }
            size_t slot{};
            while (slot < Opens && opens[slot].live) ++slot;
            reply.status = slot == Opens ? STATUS_NO_MEMORY
                : opens[slot].create(request, path, slot, response);
        } else if (operation == store::Control::List) {
            char path[sizeof(request.data) + 1]{};
            if (request.value > UINT32_MAX) { reply.status = STATUS_BAD_ARGS; return; }
            if (request.size == 0) path[0] = '/';
            else if (!path_from(request, path)) { reply.status = STATUS_BAD_ARGS; return; }
            lfs_dir_t cursor{};
            int result = lfs_dir_open(volume.fs(), &cursor, path);
            const bool opened = result == 0;
            if (result == 0 && request.value != 0)
                result = lfs_dir_seek(volume.fs(), &cursor, request.value);
            lfs_info entry{};
            while (result == 0) {
                const auto position = lfs_dir_tell(volume.fs(), &cursor);
                if (position < 0) { result = position; break; }
                result = lfs_dir_read(volume.fs(), &cursor, &entry);
                if (result <= 0) { reply.value = 0; break; }
                if (service::equal(entry.name, ".") || service::equal(entry.name, "..")) {
                    result = 0; continue;
                }
                const size_t length = service::length(entry.name);
                if (length > sizeof(reply.data)) { result = LFS_ERR_NAMETOOLONG; break; }
                const size_t separator = reply.size != 0;
                if (length + separator > sizeof(reply.data) - reply.size) {
                    reply.value = position;
                    result = 0;
                    break;
                }
                if (separator) reply.data[reply.size++] = '\0';
                service::copy(reply.data + reply.size, entry.name, length);
                reply.size += length;
                const auto next = lfs_dir_tell(volume.fs(), &cursor);
                if (next < 0) { result = next; break; }
                reply.value = next;
                result = 0;
            }
            const int closed = opened ? lfs_dir_close(volume.fs(), &cursor) : 0;
            reply.status = volume.error(result < 0 ? result : closed);
        } else if (operation == store::Control::Remove
            || operation == store::Control::Mkdir
            || operation == store::Control::Rename) {
            if (!writable) { reply.status = STATUS_DENIED; return; }
            char path[sizeof(request.data) + 1]{};
            if (!path_from(request, path)) { reply.status = STATUS_BAD_ARGS; return; }
            int result{};
            if (operation == store::Control::Remove) result = lfs_remove(volume.fs(), path);
            else if (operation == store::Control::Mkdir) result = lfs_mkdir(volume.fs(), path);
            else {
                // Rename uses a second NUL-terminated path in the same
                // immutable control frame, after the first one.
                const size_t first = service::length(path);
                if (first == 0 || first + 1 >= request.size
                    || request.data[first] != '\0') {
                    reply.status = STATUS_BAD_ARGS; return;
                }
                for (size_t i = first + 1; i < request.size; ++i)
                    if (request.data[i] == '\0') {
                        reply.status = STATUS_BAD_ARGS; return;
                    }
                const char* target = path + first + 1;
                const auto source = lookup(path);
                auto* moved = source ? find(*source) : nullptr;
                if (!source && source.error() != LFS_ERR_ISDIR) result = source.error();
                else {
                    result = lfs_rename(volume.fs(), path, target);
                    if (result == 0 && moved) {
                        const auto entry = lookup(target);
                        if (!entry) result = entry.error();
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
        } else reply.status = STATUS_INVALID_OP;
        if (volume.failed()) exit(volume.error(-1));
        reap();
    }

    static auto path_from(const io::ControlMessage& request,
        char (&out)[sizeof(io::ControlMessage::data) + 1]) noexcept -> bool {
        if (request.size == 0 || request.size > sizeof(request.data)) return false;
        service::copy(out, request.data, request.size);
        out[request.size] = '\0';
        for (size_t i = 0; i < request.size; ++i)
            if (out[i] == '\0' && request.operation != static_cast<uint64_t>(store::Control::Rename))
                return false;
        return true;
    }
};
Client clients[Clients];
} // namespace

extern "C" [[noreturn]] void user_main(const void* address, word_t size, const char* arg_data, size_t arg_size) noexcept {
    using namespace sys;
    const auto info = service::bootstrap(address, size);
    boot::Args args;
    if (!args.decode(arg_data, arg_size)) exit(STATUS_BAD_ARGS);
    const auto pool = service::capability(info, BOOT_POOL);
    const auto vspace = service::capability(info, BOOT_VSPACE);
    const auto cspace = service::capability(info, BOOT_CSPACE);
    store_pool = pool;
    store_cspace = cspace;
    store_vspace = vspace;
    events = service::capability(info, BOOT_EVENTS);
    if (args.count() > 1) exit(STATUS_BAD_ARGS);
    if (args.count() == 1) {
        const auto* text = args.argument(0);
        if (!store::parse_volume_id(text, service::length(text), expected_id))
            exit(STATUS_BAD_ARGS);
        require_id = true;
    }
    service::require(backend.connect(service::capability(info, boot::Block),
        pool, cspace, events, vspace, 0x70000000));
    service::require(volume.open(backend));
    if (require_id && volume.mounted()) {
        uint8_t actual[store::VolumeIdSize]{};
        service::require(volume.identity(actual));
        if (!expected(actual)) exit(STATUS_DENIED);
    }
    service::require(directory.open(service::capability(info, boot::Store), events));
    for (size_t i = 0; i < Clients; ++i)
        service::require(clients[i].session.prepare(pool, vspace, cspace,
            0x71000000 + i * 0x100000, true));
    service::require(notification_signal(
        service::capability(info, BOOT_READY)).status);
    service::require(notification_signal(
        service::capability(info, boot::ServiceWake)).status);
    size_t turn{};
    for (;;) {
        bool again = pump_reads();
        for (size_t count = 0; count < Clients; ++count) {
            io::ControlPacket packet;
            const auto status = directory.receive(packet);
            if (status == STATUS_WOULD_BLOCK || status == STATUS_BUSY) break;
            if (status != STATUS_OK || packet.count != 2) continue;
            auto endpoint = std::move(packet.capabilities[0]);
            if (packet.badge != store::ReadDirectory && packet.badge != store::WriteDirectory
                && packet.badge != store::AdminDirectory) {
                const io::ControlMessage reply{.operation = packet.message.operation,
                    .id = packet.message.id, .status = STATUS_DENIED};
                (void)io::ControlPort::send_to(endpoint.selector(), reply);
                (void)channel_close(endpoint.selector());
                continue;
            }
            packet.capabilities[0] = std::move(packet.capabilities[1]);
            packet.count = 1;
            Client* available{};
            for (auto& client : clients) if (!client.channel) { available = &client; break; }
            if (!available) {
                const io::ControlMessage reply{.operation = packet.message.operation,
                    .id = packet.message.id, .status = STATUS_NO_MEMORY};
                (void)io::ControlPort::send_to(endpoint.selector(), reply);
                (void)channel_close(endpoint.selector());
                continue;
            }
            auto& client = *available;
            client.writable = packet.badge != store::ReadDirectory;
            client.admin = packet.badge == store::AdminDirectory;
            client.channel = std::move(endpoint);
            const auto bound = client.session.bind(client.channel.selector(), events);
            const auto accepted = bound == STATUS_OK
                ? client.session.accept(packet, [&](const io::ControlMessage& request,
                    io::ControlReply& reply) { client.control(request, reply); }) : bound;
            if (accepted != STATUS_OK) client.session.abort();
        }
        for (auto& client : clients) if (client.channel) {
            if (client.session.poll([&](const io::ControlMessage& request,
                io::ControlReply& reply) { again = true; client.control(request, reply); }) != STATUS_OK) {
                client.session.abort(); again = true;
            }
        }
        for (auto& opened : opens) if (opened.live) {
            if (opened.session.poll([&](const io::ControlMessage& request,
                io::ControlReply& reply) { again = true; opened.control(request, reply); }) != STATUS_OK) {
                opened.session.abort(); again = true;
            }
        }
        for (size_t row = 0; row < io::QueueDepth; ++row)
            for (size_t n = 0; n < Clients; ++n) {
                auto& client = clients[(turn + n) % Clients];
                auto* queue = client.session.queue();
                if (!client.channel || !queue || client.session.closing()) continue;
                io::Ticket ticket{};
                const auto admission = queue->admit(ticket);
                if (admission == io::Admission::Empty || admission == io::Admission::Backpressure) continue;
                if (admission != io::Admission::Ready) { client.session.abort(); again = true; continue; }
                if (!queue->finish(ticket, STATUS_INVALID_OP, 0)) {
                    client.session.abort();
                    if (!queue->abandon(ticket)) exit(STATUS_INTERNAL);
                    again = true;
                }
            }
        turn = (turn + 1) % Clients;
        for (auto& opened : opens) if (opened.live) {
            auto* queue = opened.session.queue();
            if (!queue || opened.session.closing()) continue;
            for (size_t row = 0; row < io::QueueDepth && !opened.busy; ++row) {
                auto* pending = slot();
                if (!pending) break;
                io::Ticket ticket{};
                const auto admission = queue->admit(ticket);
                if (admission == io::Admission::Empty || admission == io::Admission::Backpressure) break;
                if (admission != io::Admission::Ready) {
                    opened.session.abort(); again = true; break;
                }
                if (start_read(opened, ticket, *pending)) { again = true; continue; }
                // Native inline/dirty-cache I/O and mutations retain littlefs'
                // cursor/cache ordering and cannot recycle a pinned data block.
                drain_reads();
                const auto completion = opened.transfer(*queue->request(ticket));
                // Nested Block waits share this notification and may consume
                // another session's hint. Drain all queues again before sleep.
                again = true;
                if (!queue->finish(ticket, completion.status, completion.bytes)) {
                    opened.session.abort();
                    if (!queue->abandon(ticket)) exit(STATUS_INTERNAL);
                    again = true; break;
                }
            }
        }
        again |= pump_reads();
        for (auto& client : clients) if (client.channel) {
            if (client.session.flush(true) != STATUS_OK) { client.session.abort(); again = true; }
            if (client.session.done()) {
                (void)channel_close(client.channel.selector());
                client.channel = {};
                client.session.reset();
            } else if (client.session.arm() != STATUS_OK) { client.session.abort(); again = true; }
        }
        for (auto& opened : opens) if (opened.live) {
            if (opened.session.flush(true) != STATUS_OK) {
                opened.session.abort(); again = true;
            }
            if (opened.session.done()) {
                again = true;
                --opened.file->refs;
                reap();
                opened.file = nullptr;
                (void)channel_close(opened.channel.selector());
                opened.channel = {};
                opened.session.reset();
                opened.live = false;
            } else if (opened.session.arm() != STATUS_OK) {
                opened.session.abort(); again = true;
            }
        }
        service::require(directory.arm());
        if (!again) service::require(notification_wait(events).status);
    }
}
