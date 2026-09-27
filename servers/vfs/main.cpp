#include <user/lib/file_client.hpp>
#include <user/lib/store_client.hpp>
#include <user/lib/volume_path.hpp>
#include <user/lib/vfs_protocol.hpp>

namespace {
using namespace myos;
constexpr size_t Clients = 4, Handles = 2;
io::ControlPort directory;
files::Client boot;
store::Client data;
myos_cap_t events;
bool writable_root;

struct Handle final {
    uint64_t generation{};
    store::File file{};
    bool live{}, boot{}, readable{}, writable{};
};

struct Client final {
    cap::OwnedCap channel;
    io::ServerSession session;
    Handle handles[Handles]{};
    bool writable{};

    [[nodiscard]] auto resolve(uint64_t token) noexcept -> Handle* {
        auto& entry = handles[token % Handles];
        return entry.live && entry.generation == token / Handles ? &entry : nullptr;
    }
    void close_handles() noexcept {
        for (auto& entry : handles) if (entry.live) {
            const auto status = entry.boot
                ? boot.close({entry.file.handle, entry.file.size}) : data.close(entry.file);
            service::require(status);
            entry.live = false;
        }
    }
    void control(const io::ControlMessage& request, io::ControlReply& response) noexcept {
        auto& reply = response.message;
        const auto operation = static_cast<store::Control>(request.operation);
        char path[sizeof(request.data) + 1]{};
        if (operation == store::Control::Open || operation == store::Control::Remove
            || operation == store::Control::Mkdir || operation == store::Control::Rename
            || (operation == store::Control::List && request.size != 0)) {
            if (request.size == 0 || request.size > sizeof(request.data)) {
                reply.status = MYOS_STATUS_BAD_ARGS; return;
            }
            service::copy(path, request.data, request.size);
            for (size_t i = 0; i < request.size; ++i)
                if (path[i] == '\0' && operation != store::Control::Rename) {
                    reply.status = MYOS_STATUS_BAD_ARGS; return;
                }
        }
        if (operation == store::Control::Open) {
            constexpr uint64_t allowed = store::Read | store::Write | store::Create
                | store::Truncate | store::Exclusive;
            if (request.value == 0 || (request.value & ~allowed) != 0
                || ((request.value & (store::Create | store::Truncate | store::Exclusive)) != 0
                    && (request.value & store::Write) == 0)) {
                reply.status = MYOS_STATUS_BAD_ARGS; return;
            }
            const char* name = volume_path::boot_name(path);
            const bool on_boot = name != nullptr || !writable_root;
            if ((request.value & store::Write) != 0 && !writable) {
                reply.status = MYOS_STATUS_DENIED; return;
            }
            if (on_boot && (request.value != store::Read)) {
                reply.status = MYOS_STATUS_DENIED; return;
            }
            if (on_boot) {
                name = name != nullptr ? name : path;
                if (*name == '\0' || service::length(name) > sizeof(request.data)) {
                    reply.status = MYOS_STATUS_BAD_ARGS; return;
                }
            }
            size_t slot{};
            while (slot < Handles && (handles[slot].live
                || handles[slot].generation == UINT64_MAX / Handles)) ++slot;
            if (slot == Handles) { reply.status = MYOS_STATUS_NO_MEMORY; return; }
            store::File opened{};
            if (on_boot) {
                files::File file{};
                reply.status = boot.open(name, service::length(name), file);
                opened = {file.handle, file.size};
            } else reply.status = data.open(path, request.value, opened);
            if (reply.status != MYOS_STATUS_OK) return;
            auto& entry = handles[slot];
            entry = {entry.generation + 1, opened, true, on_boot,
                (request.value & store::Read) != 0, (request.value & store::Write) != 0};
            reply.value = entry.generation * Handles + slot;
            reply.size = sizeof(uint64_t);
            service::copy(reply.data, &opened.size, reply.size);
        } else if (operation == store::Control::Close || operation == store::Control::Sync) {
            auto* entry = resolve(request.value);
            if (request.size != 0 || entry == nullptr) { reply.status = MYOS_STATUS_BAD_ARGS; return; }
            if (operation == store::Control::Sync) {
                reply.status = entry->boot ? MYOS_STATUS_OK : data.sync(entry->file);
            } else {
                reply.status = entry->boot
                    ? boot.close({entry->file.handle, entry->file.size}) : data.close(entry->file);
                if (reply.status == MYOS_STATUS_OK) entry->live = false;
            }
        } else if (operation == store::Control::List) {
            const char* name = request.size == 0 ? "/" : path;
            const char* boot_name = volume_path::boot_name(name);
            const bool boot_dir = boot_name != nullptr || !writable_root;
            if (boot_dir && boot_name != nullptr && *boot_name != '\0') {
                reply.status = MYOS_STATUS_NOT_FOUND; return;
            }
            const bool root = service::equal(name, "/") || service::equal(name, "");
            if (root && request.value == 0) {
                service::copy(reply.data, "boot", 4);
                reply.size = 4;
                reply.value = UINT64_MAX;
                return;
            }
            const uint64_t cursor = root && request.value == UINT64_MAX
                ? 0 : request.value;
            io::ControlMessage entry{.value = cursor};
            if (boot_dir) reply.status = boot.list(entry);
            else do {
                reply.status = data.list(entry, name);
                if (reply.status != MYOS_STATUS_OK || !root || entry.size != 4
                    || entry.data[0] != 'b' || entry.data[1] != 'o'
                    || entry.data[2] != 'o' || entry.data[3] != 't') break;
                if (entry.value == 0) { entry.size = 0; break; }
            } while (true);
            if (reply.status == MYOS_STATUS_OK) {
                if (boot_dir && entry.size != 0) {
                    while (reply.size < entry.size && entry.data[reply.size] != '\n')
                        ++reply.size;
                    if (reply.size == entry.size) {
                        reply.status = MYOS_STATUS_PEER_FAULT; return;
                    }
                    reply.value = reply.size + 1 < entry.size || entry.value != 0
                        ? cursor + 1 : 0;
                } else {
                    reply.size = entry.size;
                    reply.value = entry.value;
                }
                service::copy(reply.data, entry.data, reply.size);
            }
        } else if (operation == store::Control::Remove || operation == store::Control::Mkdir) {
            if (!writable || !writable_root || volume_path::boot_name(path) != nullptr) {
                reply.status = MYOS_STATUS_DENIED; return;
            }
            reply.status = operation == store::Control::Remove
                ? data.remove(path) : data.mkdir(path);
        } else if (operation == store::Control::Rename) {
            const size_t first = service::length(path);
            if (first == 0 || first + 1 >= request.size || request.data[first] != '\0') {
                reply.status = MYOS_STATUS_BAD_ARGS; return;
            }
            const char* target = path + first + 1;
            if (*target == '\0' || service::length(target) != request.size - first - 1) {
                reply.status = MYOS_STATUS_BAD_ARGS; return;
            }
            reply.status = !writable || !writable_root || volume_path::boot_name(path) != nullptr
                || volume_path::boot_name(target) != nullptr ? MYOS_STATUS_DENIED
                : data.rename(path, target);
        } else if (operation == store::Control::DeviceId
            || operation == store::Control::VolumeId) {
            if (request.size != 0 || request.value != 0) {
                reply.status = MYOS_STATUS_BAD_ARGS; return;
            }
            if (!writable_root) { reply.status = MYOS_STATUS_NOT_FOUND; return; }
            if (operation == store::Control::DeviceId) {
                uint8_t id[20]{};
                reply.status = data.device_id(id);
                if (reply.status == MYOS_STATUS_OK) {
                    reply.size = sizeof(id);
                    service::copy(reply.data, id, reply.size);
                }
            } else {
                uint8_t id[store::VolumeIdSize]{};
                reply.status = data.volume_id(id);
                if (reply.status == MYOS_STATUS_OK) {
                    reply.size = sizeof(id);
                    service::copy(reply.data, id, reply.size);
                }
            }
        } else reply.status = MYOS_STATUS_INVALID_OP;
    }
    [[nodiscard]] auto transfer(const io::Request& request) noexcept -> io::Completion {
        if (request.buffer != 0 || request.flags != 0 || request.length == 0
            || request.length > io::BufferSize || request.buffer_offset > io::PayloadSize
            || request.length > io::PayloadSize - request.buffer_offset)
            return {request.id, MYOS_STATUS_BAD_ARGS, 0, 0};
        auto* entry = resolve(request.object);
        if (entry == nullptr) return {request.id, MYOS_STATUS_NOT_FOUND, 0, 0};
        auto* payload = session.payload() + request.buffer_offset;
        if (request.operation == static_cast<uint64_t>(io::Operation::Read)) {
            if (!entry->readable) return {request.id, MYOS_STATUS_DENIED, 0, 0};
            uint64_t bytes{};
            const auto status = entry->boot
                ? boot.read_at({entry->file.handle, entry->file.size}, request.offset,
                    payload, request.length, bytes)
                : data.read_at(entry->file, request.offset, payload, request.length, bytes);
            return {request.id, status, bytes, 0};
        }
        if (request.operation != static_cast<uint64_t>(io::Operation::Write))
            return {request.id, MYOS_STATUS_INVALID_OP, 0, 0};
        if (!entry->writable) return {request.id, MYOS_STATUS_DENIED, 0, 0};
        const auto status = data.write(entry->file, request.offset, payload, request.length);
        return {request.id, status, status == MYOS_STATUS_OK ? request.length : 0, 0};
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
    service::require(boot.connect(info));
    writable_root = info.selector(bootstrap::imports::Store) != 0;
    if (writable_root) service::require(data.connect(info));
    service::require(directory.open(service::capability(info, bootstrap::imports::Vfs), events));
    for (size_t i = 0; i < Clients; ++i)
        service::require(clients[i].session.prepare(pool, vspace, cspace,
            0x71000000 + i * 0x100000, true));
    service::require(notification_signal(
        service::capability(info, MYOS_BOOTSTRAP_CAP_READINESS_NOTIFICATION)).status);
    service::require(notification_signal(
        service::capability(info, bootstrap::imports::ServiceWake)).status);
    for (;;) {
        bool again{};
        for (size_t count = 0; count < Clients; ++count) {
            io::ControlPacket packet;
            const auto status = directory.receive(packet);
            if (status == MYOS_STATUS_WOULD_BLOCK || status == MYOS_STATUS_BUSY) break;
            if (status != MYOS_STATUS_OK || packet.count != 2) continue;
            auto endpoint = libk::move(packet.capabilities[0]);
            packet.capabilities[0] = libk::move(packet.capabilities[1]);
            packet.count = 1;
            Client* available{};
            for (auto& client : clients) if (!client.channel) { available = &client; break; }
            if ((packet.badge != vfs::ReadDirectory
                && packet.badge != vfs::WriteDirectory) || available == nullptr) {
                const io::ControlMessage reply{.operation = packet.message.operation,
                    .id = packet.message.id,
                    .status = packet.badge != vfs::ReadDirectory
                        && packet.badge != vfs::WriteDirectory
                        ? MYOS_STATUS_DENIED : MYOS_STATUS_NO_MEMORY};
                (void)io::ControlPort::send_to(endpoint.selector(), reply);
                (void)channel_close(endpoint.selector());
                continue;
            }
            available->writable = packet.badge == vfs::WriteDirectory;
            available->channel = libk::move(endpoint);
            const auto bound = available->session.bind(available->channel.selector(), events);
            const auto accepted = bound == MYOS_STATUS_OK
                ? available->session.accept(packet, [&](const io::ControlMessage& request,
                    io::ControlReply& reply) { available->control(request, reply); }) : bound;
            if (accepted != MYOS_STATUS_OK) available->session.abort();
        }
        for (auto& client : clients) if (client.channel) {
            if (client.session.poll([&](const io::ControlMessage& request,
                io::ControlReply& reply) { client.control(request, reply); }) != MYOS_STATUS_OK) {
                client.session.abort(); again = true;
            }
        }
        for (auto& client : clients) if (client.channel) {
            auto* queue = client.session.queue();
            if (queue == nullptr || client.session.closing()) continue;
            for (size_t n = 0; n < io::QueueDepth; ++n) {
                io::Ticket ticket{};
                const auto admitted = queue->admit(ticket);
                if (admitted == io::Admission::Empty || admitted == io::Admission::Backpressure) break;
                if (admitted != io::Admission::Ready) { client.session.abort(); again = true; break; }
                const auto completion = client.transfer(*queue->request(ticket));
                if (!queue->finish(ticket, completion.status, completion.bytes)) {
                    client.session.abort();
                    if (!queue->abandon(ticket)) exit(MYOS_STATUS_INTERNAL);
                    again = true;
                }
            }
        }
        for (auto& client : clients) if (client.channel) {
            if (client.session.flush(true) != MYOS_STATUS_OK) { client.session.abort(); again = true; }
            if (client.session.done()) {
                client.close_handles();
                (void)channel_close(client.channel.selector());
                client.channel = {};
                client.session.reset();
            } else if (client.session.arm() != MYOS_STATUS_OK) { client.session.abort(); again = true; }
        }
        service::require(directory.arm());
        if (!again) service::require(notification_wait(events).status);
    }
}
