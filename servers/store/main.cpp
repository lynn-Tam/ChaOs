#include <servers/store/volume.hpp>
#include <user/lib/imports.hpp>
#include <user/lib/store_protocol.hpp>

namespace {
using namespace myos;
constexpr size_t Clients = 4;
constexpr size_t Files = 8;
io::ControlPort directory;
block::Client backend;
store::Volume volume;
myos_cap_t events;
uint8_t expected_id[store::VolumeIdSize]{};
bool require_id{};

auto expected(const uint8_t* id) noexcept -> bool {
    for (size_t i = 0; i < sizeof(expected_id); ++i)
        if (id[i] != expected_id[i]) return false;
    return true;
}

struct File final {
    lfs_file_t state{};
    lfs_file_config config{};
    uint8_t cache[512]{};
    char name[sizeof(io::ControlMessage::data) + 1]{};
    uint64_t generation{};
    bool open{};
    bool writable{};
    bool readable{};
};

struct Client;
extern Client clients[Clients];
struct Client final {
    cap::OwnedCap channel;
    io::ServerSession session;
    File files[Files]{};
    bool writable{};
    bool admin{};

    [[nodiscard]] auto file(uint64_t token) noexcept -> File* {
        auto& candidate = files[token % Files];
        return candidate.open && candidate.generation == token / Files ? &candidate : nullptr;
    }

    void close_files() noexcept {
        for (auto& file : files) if (file.open) {
            const int result = lfs_file_close(volume.fs(), &file.state);
            file.open = false;
            if (result < 0) exit(volume.error(result));
        }
    }

    void control(const io::ControlMessage& request, io::ControlReply& response) noexcept {
        auto& reply = response.message;
        const auto operation = static_cast<store::Control>(request.operation);
        if (operation == store::Control::DeviceId) {
            if (request.size != 0 || request.value != 0) {
                reply.status = MYOS_STATUS_BAD_ARGS; return;
            }
            uint8_t id[20]{};
            reply.status = backend.identify(id);
            if (reply.status == MYOS_STATUS_BACKING_FAILED) exit(reply.status);
            if (reply.status == MYOS_STATUS_OK) {
                reply.size = sizeof(id);
                service::copy(reply.data, id, sizeof(id));
            }
            return;
        }
        if (operation == store::Control::VolumeId) {
            if (request.size != 0 || request.value != 0) {
                reply.status = MYOS_STATUS_BAD_ARGS; return;
            }
            uint8_t id[store::VolumeIdSize]{};
            reply.status = volume.identity(id);
            if (volume.failed()) exit(volume.error(-1));
            if (reply.status == MYOS_STATUS_OK) {
                reply.size = sizeof(id);
                service::copy(reply.data, id, sizeof(id));
            }
            return;
        }
        if (operation == store::Control::Format) {
            if (!admin) { reply.status = MYOS_STATUS_DENIED; return; }
            if ((request.size != 0 && request.size != store::VolumeIdSize) || request.value != 0) {
                reply.status = MYOS_STATUS_BAD_ARGS; return;
            }
            if (require_id && (request.size != store::VolumeIdSize
                || !expected(reinterpret_cast<const uint8_t*>(request.data)))) {
                reply.status = MYOS_STATUS_DENIED; return;
            }
            for (auto& owner : clients) for (auto& opened : owner.files)
                if (opened.open) { reply.status = MYOS_STATUS_BUSY; return; }
            reply.status = volume.format(request.size == 0
                ? nullptr : reinterpret_cast<const uint8_t*>(request.data));
            if (volume.failed()) exit(volume.error(-1));
            return;
        }
        if (!volume.mounted()) { reply.status = MYOS_STATUS_BACKING_FAILED; return; }
        if (operation == store::Control::Sync || operation == store::Control::Close) {
            auto* opened = file(request.value);
            if (request.size != 0 || opened == nullptr) reply.status = MYOS_STATUS_BAD_ARGS;
            else {
                const int result = operation == store::Control::Sync
                    ? lfs_file_sync(volume.fs(), &opened->state)
                    : lfs_file_close(volume.fs(), &opened->state);
                if (operation == store::Control::Close) opened->open = false;
                reply.status = volume.error(result);
            }
        } else if (operation == store::Control::Open) {
            char path[sizeof(request.data) + 1]{};
            if (!path_from(request, path) || request.value == 0
                || (request.value & ~(store::Read | store::Write | store::Create
                    | store::Truncate | store::Exclusive)) != 0
                || ((request.value & (store::Create | store::Truncate | store::Exclusive)) != 0
                    && (request.value & store::Write) == 0)) {
                reply.status = MYOS_STATUS_BAD_ARGS;
                return;
            }
            const bool write = (request.value & store::Write) != 0;
            if (write && !writable) { reply.status = MYOS_STATUS_DENIED; return; }
            if (write) for (auto& other : clients) for (auto& existing : other.files)
                if (existing.open && existing.writable && service::equal(existing.name, path)) {
                    reply.status = MYOS_STATUS_BUSY;
                    return;
                }
            File* available{};
            size_t index{};
            for (size_t i = 0; i < Files; ++i) if (!files[i].open
                && files[i].generation != UINT64_MAX / Files) { available = &files[i]; index = i; break; }
            if (available == nullptr) { reply.status = MYOS_STATUS_NO_MEMORY; return; }
            auto& opened = *available;
            opened.config = {};
            opened.config.buffer = opened.cache;
            int flags = (request.value & (store::Read | store::Write)) == (store::Read | store::Write)
                ? LFS_O_RDWR : write ? LFS_O_WRONLY : LFS_O_RDONLY;
            if (request.value & store::Create) flags |= LFS_O_CREAT;
            if (request.value & store::Truncate) flags |= LFS_O_TRUNC;
            if (request.value & store::Exclusive) flags |= LFS_O_EXCL;
            const int result = lfs_file_opencfg(volume.fs(), &opened.state, path, flags,
                &opened.config);
            reply.status = volume.error(result);
            if (result == 0) {
                opened.open = true;
                opened.writable = write;
                opened.readable = (request.value & store::Read) != 0;
                service::copy(opened.name, path, request.size + 1);
                reply.value = ++opened.generation * Files + index;
                const auto size = lfs_file_size(volume.fs(), &opened.state);
                reply.status = volume.error(size < 0 ? size : 0);
                if (size >= 0) {
                    const uint64_t length = size;
                    reply.size = sizeof(uint64_t);
                    service::copy(reply.data, &length, sizeof(length));
                } else {
                    (void)lfs_file_close(volume.fs(), &opened.state);
                    opened.open = false;
                }
            }
        } else if (operation == store::Control::List) {
            char path[sizeof(request.data) + 1]{};
            if (request.value > UINT32_MAX) { reply.status = MYOS_STATUS_BAD_ARGS; return; }
            if (request.size == 0) path[0] = '/';
            else if (!path_from(request, path)) { reply.status = MYOS_STATUS_BAD_ARGS; return; }
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
            if (!writable) { reply.status = MYOS_STATUS_DENIED; return; }
            if (operation != store::Control::Mkdir)
                for (auto& other : clients) for (auto& existing : other.files)
                    if (existing.open && existing.writable) {
                        reply.status = MYOS_STATUS_BUSY;
                        return;
                    }
            char path[sizeof(request.data) + 1]{};
            if (!path_from(request, path)) { reply.status = MYOS_STATUS_BAD_ARGS; return; }
            int result{};
            if (operation == store::Control::Remove) result = lfs_remove(volume.fs(), path);
            else if (operation == store::Control::Mkdir) result = lfs_mkdir(volume.fs(), path);
            else {
                // Rename uses a second NUL-terminated path in the same
                // immutable control frame, after the first one.
                const size_t first = service::length(path);
                if (first == 0 || first + 1 >= request.size
                    || request.data[first] != '\0') {
                    reply.status = MYOS_STATUS_BAD_ARGS; return;
                }
                for (size_t i = first + 1; i < request.size; ++i)
                    if (request.data[i] == '\0') {
                        reply.status = MYOS_STATUS_BAD_ARGS; return;
                    }
                result = lfs_rename(volume.fs(), path, path + first + 1);
            }
            reply.status = volume.error(result);
        } else reply.status = MYOS_STATUS_INVALID_OP;
        if (volume.failed()) exit(volume.error(-1));
    }

    [[nodiscard]] auto transfer(const io::Request& request) noexcept -> io::Completion {
        if (request.buffer != 0 || request.flags != 0 || request.length == 0
            || request.length > io::BufferSize || request.buffer_offset > io::PayloadSize
            || request.length > io::PayloadSize - request.buffer_offset)
            return {request.id, MYOS_STATUS_BAD_ARGS, 0, 0};
        auto* opened = file(request.object);
        if (!opened) return {request.id, MYOS_STATUS_NOT_FOUND, 0, 0};
        const bool write = request.operation == static_cast<uint64_t>(io::Operation::Write);
        if (!write && request.operation != static_cast<uint64_t>(io::Operation::Read))
            return {request.id, MYOS_STATUS_INVALID_OP, 0, 0};
        if (write && !opened->writable) return {request.id, MYOS_STATUS_DENIED, 0, 0};
        if (!write && !opened->readable) return {request.id, MYOS_STATUS_DENIED, 0, 0};
        if (request.offset > LFS_FILE_MAX
            || request.length > LFS_FILE_MAX - request.offset)
            return {request.id, MYOS_STATUS_BAD_ARGS, 0, 0};
        const auto seek = lfs_file_seek(volume.fs(), &opened->state, request.offset, LFS_SEEK_SET);
        if (seek < 0) {
            const auto status = volume.error(seek);
            if (volume.failed()) exit(status);
            return {request.id, status, 0, 0};
        }
        auto* data = session.payload() + request.buffer_offset;
        const auto result = write
            ? lfs_file_write(volume.fs(), &opened->state, data, request.length)
            : lfs_file_read(volume.fs(), &opened->state, data, request.length);
        if (volume.failed()) exit(volume.error(result));
        return {request.id, volume.error(result < 0 ? result : 0),
            result > 0 ? static_cast<uint64_t>(result) : 0, 0};
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

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    using namespace myos;
    const auto info = service::bootstrap(address, size);
    const auto pool = service::capability(info, MYOS_BOOTSTRAP_CAP_RESOURCE_POOL);
    const auto vspace = service::capability(info, MYOS_BOOTSTRAP_CAP_VSPACE);
    const auto cspace = service::capability(info, MYOS_BOOTSTRAP_CAP_CSPACE);
    events = service::capability(info, MYOS_BOOTSTRAP_CAP_SERVICE_NOTIFICATION);
    if (info.argument_count() > 1) exit(MYOS_STATUS_BAD_ARGS);
    if (info.argument_count() == 1) {
        const auto* text = info.argument(0);
        if (!store::parse_volume_id(text, service::length(text), expected_id))
            exit(MYOS_STATUS_BAD_ARGS);
        require_id = true;
    }
    service::require(backend.connect(service::capability(info, bootstrap::imports::Block),
        pool, cspace, events, vspace, 0x70000000));
    service::require(volume.open(backend));
    if (require_id && volume.mounted()) {
        uint8_t actual[store::VolumeIdSize]{};
        service::require(volume.identity(actual));
        if (!expected(actual)) exit(MYOS_STATUS_DENIED);
    }
    service::require(directory.open(service::capability(info, bootstrap::imports::Store), events));
    for (size_t i = 0; i < Clients; ++i)
        service::require(clients[i].session.prepare(pool, vspace, cspace,
            0x71000000 + i * 0x100000, true));
    service::require(notification_signal(
        service::capability(info, MYOS_BOOTSTRAP_CAP_READINESS_NOTIFICATION)).status);
    service::require(notification_signal(
        service::capability(info, bootstrap::imports::ServiceWake)).status);
    size_t turn{};
    for (;;) {
        bool again{};
        for (size_t count = 0; count < Clients; ++count) {
            io::ControlPacket packet;
            const auto status = directory.receive(packet);
            if (status == MYOS_STATUS_WOULD_BLOCK || status == MYOS_STATUS_BUSY) break;
            if (status != MYOS_STATUS_OK || packet.count != 2) continue;
            auto endpoint = libk::move(packet.capabilities[0]);
            if (packet.badge != store::ReadDirectory && packet.badge != store::WriteDirectory
                && packet.badge != store::AdminDirectory) {
                const io::ControlMessage reply{.operation = packet.message.operation,
                    .id = packet.message.id, .status = MYOS_STATUS_DENIED};
                (void)io::ControlPort::send_to(endpoint.selector(), reply);
                (void)channel_close(endpoint.selector());
                continue;
            }
            packet.capabilities[0] = libk::move(packet.capabilities[1]);
            packet.count = 1;
            Client* available{};
            for (auto& client : clients) if (!client.channel) { available = &client; break; }
            if (!available) {
                const io::ControlMessage reply{.operation = packet.message.operation,
                    .id = packet.message.id, .status = MYOS_STATUS_NO_MEMORY};
                (void)io::ControlPort::send_to(endpoint.selector(), reply);
                (void)channel_close(endpoint.selector());
                continue;
            }
            auto& client = *available;
            client.writable = packet.badge != store::ReadDirectory;
            client.admin = packet.badge == store::AdminDirectory;
            client.channel = libk::move(endpoint);
            const auto bound = client.session.bind(client.channel.selector(), events);
            const auto accepted = bound == MYOS_STATUS_OK
                ? client.session.accept(packet, [&](const io::ControlMessage& request,
                    io::ControlReply& reply) { client.control(request, reply); }) : bound;
            if (accepted != MYOS_STATUS_OK) client.session.abort();
        }
        for (auto& client : clients) if (client.channel) {
            if (client.session.poll([&](const io::ControlMessage& request,
                io::ControlReply& reply) { client.control(request, reply); }) != MYOS_STATUS_OK) {
                client.session.abort(); again = true;
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
                const auto completion = client.transfer(*queue->request(ticket));
                if (!queue->finish(ticket, completion.status, completion.bytes)) {
                    client.session.abort();
                    if (!queue->abandon(ticket)) exit(MYOS_STATUS_INTERNAL);
                    again = true;
                }
            }
        turn = (turn + 1) % Clients;
        for (auto& client : clients) if (client.channel) {
            if (client.session.flush(true) != MYOS_STATUS_OK) { client.session.abort(); again = true; }
            if (client.session.done()) {
                client.close_files();
                (void)channel_close(client.channel.selector());
                client.channel = {};
                client.session.reset();
            } else if (client.session.arm() != MYOS_STATUS_OK) { client.session.abort(); again = true; }
        }
        service::require(directory.arm());
        if (!again) service::require(notification_wait(events).status);
    }
}
