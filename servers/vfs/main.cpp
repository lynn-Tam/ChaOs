#include <string_view>
#include <string.h>
#include <utility>
#include <sys/channel.hpp>
#include <sys/queue.hpp>
#include <sys/protocol.hpp>

namespace {
// The boot mount is read-only and flat. Only this exact component selects it.
[[nodiscard]] inline auto boot_name(const char* path) noexcept -> const char* {
    if (path == nullptr) return nullptr;
    constexpr char prefix[] = "/boot";
    for (size_t i = 0; i < 5; ++i) if (path[i] != prefix[i]) return nullptr;
    if (path[5] == '\0') return path + 5;
    return path[5] == '/' ? path + 6 : nullptr;
}


using namespace sys;
constexpr size_t Clients = 4;
io::ControlPort directory;
io::ClientSession boot_files;
io::ClientSession data;
cap_t events;
bool writable_root;

struct Client final {
    cap_owner channel;
    io::ServerSession session;
    bool writable{};

    void control(const io::ControlMessage& request, io::ControlReply& response) noexcept {
        auto& reply = response.message;
        const auto operation = static_cast<store::Control>(request.operation);
        char path[sizeof(request.data) + 1]{};
        if (operation == store::Control::Open
            || operation == store::Control::Remove
            || operation == store::Control::Mkdir || operation == store::Control::Rename
            || (operation == store::Control::List && request.size != 0)) {
            if (request.size == 0 || request.size > sizeof(request.data)) {
                reply.status = STATUS_BAD_ARGS; return;
            }
            ::memcpy(path, request.data, request.size);
            for (size_t i = 0; i < request.size; ++i)
                if (path[i] == '\0' && operation != store::Control::Rename) {
                    reply.status = STATUS_BAD_ARGS; return;
                }
        }
        if (operation == store::Control::Open) {
            constexpr uint64_t allowed = store::Read | store::Write | store::Create
                | store::Truncate | store::Exclusive | store::Append;
            if (request.value == 0 || (request.value & ~allowed) != 0
                || ((request.value & (store::Create | store::Truncate | store::Exclusive | store::Append)) != 0
                    && (request.value & store::Write) == 0)) {
                reply.status = STATUS_BAD_ARGS; return;
            }
            const char* name = boot_name(path);
            const bool on_boot = name != nullptr || !writable_root;
            if ((request.value & store::Write) != 0 && !writable) {
                reply.status = STATUS_DENIED; return;
            }
            if (on_boot && (request.value != store::Read)) {
                reply.status = STATUS_DENIED; return;
            }
            if (on_boot) {
                name = name != nullptr ? name : path;
                if (*name == '\0' || ::strlen(name) > sizeof(request.data)) {
                    reply.status = STATUS_BAD_ARGS; return;
                }
            }
            cap_owner object;
            uint64_t length{}, generation{};
            reply.status = on_boot
                ? fs::open_object(boot_files, name, ::strlen(name), object, length, generation)
                : store::open(data, path, request.value, object, length, generation);
            if (reply.status != STATUS_OK) return;
            constexpr auto rights = RIGHT_SEND | RIGHT_RECEIVE
                | RIGHT_CLOSE | RIGHT_DESTROY;
            if (!response.offer(std::move(object), rights)) {
                reply.status = STATUS_INTERNAL; return;
            }
            reply.value = generation;
            reply.size = sizeof(length);
            ::memcpy(reply.data, &length, sizeof(length));
        } else if (operation == store::Control::List) {
            const char* name = request.size == 0 ? "/" : path;
            const char* tail = boot_name(name);
            const bool boot_dir = tail != nullptr || !writable_root;
            if (boot_dir && tail != nullptr && *tail != '\0') {
                reply.status = STATUS_NOT_FOUND; return;
            }
            const bool root = (std::string_view{name} == "/") || (std::string_view{name} == "");
            if (root && request.value == 0) {
                ::memcpy(reply.data, "boot", 4);
                reply.size = 4;
                reply.value = UINT64_MAX;
                return;
            }
            const uint64_t cursor = root && request.value == UINT64_MAX
                ? 0 : request.value;
            io::ControlMessage entry{.value = cursor};
            if (boot_dir) reply.status = fs::list(boot_files, entry);
            else do {
                reply.status = store::list(data, entry, name);
                if (reply.status != STATUS_OK || !root) break;
                size_t start{};
                reply.size = 0;
                while (start < entry.size) {
                    size_t end = start;
                    while (end < entry.size && entry.data[end] != '\0') ++end;
                    if (end - start != 4 || entry.data[start] != 'b'
                        || entry.data[start + 1] != 'o' || entry.data[start + 2] != 'o'
                        || entry.data[start + 3] != 't') {
                        if (reply.size != 0) reply.data[reply.size++] = '\0';
                        ::memcpy(reply.data + reply.size, entry.data + start, end - start);
                        reply.size += end - start;
                    }
                    start = end + 1;
                }
                if (reply.size != 0 || entry.value == 0) break;
                entry = {.value = entry.value};
            } while (true);
            if (reply.status == STATUS_OK) {
                if (boot_dir && entry.size != 0) {
                    reply.size = entry.size;
                    ::memcpy(reply.data, entry.data, reply.size);
                    if (reply.data[reply.size - 1] != '\n') {
                        reply.status = STATUS_PEER_FAULT; return;
                    }
                    for (size_t i = 0; i < reply.size; ++i)
                        if (reply.data[i] == '\n') reply.data[i] = '\0';
                    --reply.size;
                    reply.value = entry.value;
                } else {
                    if (!root) {
                        reply.size = entry.size;
                        ::memcpy(reply.data, entry.data, reply.size);
                    }
                    reply.value = entry.value;
                }
            }
        } else if (operation == store::Control::Remove || operation == store::Control::Mkdir) {
            if (!writable || !writable_root || boot_name(path) != nullptr) {
                reply.status = STATUS_DENIED; return;
            }
            reply.status = operation == store::Control::Remove
                ? store::remove(data, path) : store::mkdir(data, path);
        } else if (operation == store::Control::Rename) {
            const size_t first = ::strlen(path);
            if (first == 0 || first + 1 >= request.size || request.data[first] != '\0') {
                reply.status = STATUS_BAD_ARGS; return;
            }
            const char* target = path + first + 1;
            if (*target == '\0' || ::strlen(target) != request.size - first - 1) {
                reply.status = STATUS_BAD_ARGS; return;
            }
            reply.status = !writable || !writable_root || boot_name(path) != nullptr
                || boot_name(target) != nullptr ? STATUS_DENIED
                : store::rename(data, path, target);
        } else if (operation == store::Control::Sync) {
            if (request.size || request.value) reply.status = STATUS_BAD_ARGS;
            else if (!writable) reply.status = STATUS_DENIED;
            else if (writable_root) reply.status = store::sync(data);
        } else if (operation == store::Control::DeviceId
            || operation == store::Control::VolumeId) {
            if (request.size != 0 || request.value != 0) {
                reply.status = STATUS_BAD_ARGS; return;
            }
            if (!writable_root) { reply.status = STATUS_NOT_FOUND; return; }
            if (operation == store::Control::DeviceId) {
                uint8_t id[20]{};
                reply.status = store::device_id(data, id);
                if (reply.status == STATUS_OK) {
                    reply.size = sizeof(id);
                    ::memcpy(reply.data, id, reply.size);
                }
            } else {
                uint8_t id[store::VolumeIdSize]{};
                reply.status = store::volume_id(data, id);
                if (reply.status == STATUS_OK) {
                    reply.size = sizeof(id);
                    ::memcpy(reply.data, id, reply.size);
                }
            }
        } else reply.status = STATUS_INVALID_OP;
    }

};
Client clients[Clients];
} // namespace

extern "C" [[noreturn]] void user_main(const void* address, word_t size) noexcept {
    using namespace sys;
    const auto info = sys::bootstrap(address, size);
    const auto pool = sys::capability(info, BOOT_POOL);
    const auto vspace = sys::capability(info, BOOT_VSPACE);
    const auto cspace = sys::capability(info, BOOT_CSPACE);
    events = sys::capability(info, BOOT_EVENTS);
    uint64_t value{};
    sys::require(boot_files.connect(sys::capability(info, boot::Files),
        sys::capability(info, BOOT_POOL), sys::capability(info, BOOT_CSPACE),
        sys::capability(info, BOOT_EVENTS), sys::capability(info, BOOT_VSPACE), 0x70000000, value));
    writable_root = info.selector(boot::Store) != 0;
    if (writable_root) sys::require(data.connect(sys::capability(info, boot::Store),
        sys::capability(info, BOOT_POOL), sys::capability(info, BOOT_CSPACE),
        sys::capability(info, BOOT_EVENTS), sys::capability(info, BOOT_VSPACE), 0x76000000, value, true));
    sys::require(directory.open(sys::capability(info, boot::Vfs), events));
    for (size_t i = 0; i < Clients; ++i)
        sys::require(clients[i].session.prepare(pool, vspace, cspace,
            0x71000000 + i * 0x100000, true));
    sys::require(notification_signal(
        sys::capability(info, BOOT_READY)).status);
    sys::require(notification_signal(
        sys::capability(info, boot::ServiceWake)).status);
    for (;;) {
        bool again{};
        for (size_t count = 0; count < Clients; ++count) {
            io::ControlPacket packet;
            const auto status = directory.receive(packet);
            if (status == STATUS_WOULD_BLOCK || status == STATUS_BUSY) break;
            if (status != STATUS_OK || packet.count != 2) continue;
            auto endpoint = std::move(packet.capabilities[0]);
            packet.capabilities[0] = std::move(packet.capabilities[1]);
            packet.count = 1;
            Client* available{};
            for (auto& client : clients) if (!client.channel) { available = &client; break; }
            if ((packet.badge != store::ReadDirectory
                && packet.badge != store::WriteDirectory) || available == nullptr) {
                const io::ControlMessage reply{.operation = packet.message.operation,
                    .id = packet.message.id,
                    .status = packet.badge != store::ReadDirectory
                        && packet.badge != store::WriteDirectory
                        ? STATUS_DENIED : STATUS_NO_MEMORY};
                (void)io::ControlPort::send_to(endpoint.selector(), reply);
                (void)channel_close(endpoint.selector());
                continue;
            }
            available->writable = packet.badge == store::WriteDirectory;
            available->channel = std::move(endpoint);
            const auto bound = available->session.bind(available->channel.selector(), events);
            const auto accepted = bound == STATUS_OK
                ? available->session.accept(packet, [&](const io::ControlMessage& request,
                    io::ControlReply& reply) { available->control(request, reply); }) : bound;
            if (accepted != STATUS_OK) available->session.abort();
        }
        for (auto& client : clients) if (client.channel) {
            if (client.session.poll([&](const io::ControlMessage& request,
                io::ControlReply& reply) { again = true; client.control(request, reply); }) != STATUS_OK) {
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
                if (!queue->finish(ticket, STATUS_INVALID_OP, 0)) {
                    client.session.abort();
                    if (!queue->abandon(ticket)) exit(STATUS_INTERNAL);
                    again = true;
                }
            }
        }
        for (auto& client : clients) if (client.channel) {
            if (client.session.flush(true) != STATUS_OK) { client.session.abort(); again = true; }
            if (client.session.done()) {
                (void)channel_close(client.channel.selector());
                client.channel = {};
                client.session.reset();
            } else if (client.session.arm() != STATUS_OK) { client.session.abort(); again = true; }
        }
        sys::require(directory.arm());
        if (!again) sys::require(notification_wait(events).status);
    }
}
