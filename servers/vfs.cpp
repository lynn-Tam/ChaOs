#include <util.hpp>
#include <string_view>
#include <string.h>
#include <utility>
#include <ch.hpp>
#include <io.hpp>
#include <proto.hpp>

// The boot mount is read-only and flat. Only this exact component selects it.
[[nodiscard]] inline auto boot_name(const char* path) noexcept -> const char* {
    if (path == nullptr)
        return nullptr;
    constexpr char prefix[] = "/boot";
    for (size_t i = 0; i < 5; ++i)
        if (path[i] != prefix[i])
            return nullptr;
    if (path[5] == '\0')
        return path + 5;
    return path[5] == '/' ? path + 6 : nullptr;
}

static ch_port directory;
static io_client boot_files;
static ch_rpc data;
static cap_t events;
static bool writable_root;

static void control(const ch_msg& request, ch_reply& response, bool writable) noexcept {
    auto& reply = response.message;
    const auto operation = static_cast<st_control>(request.operation);
    char path[sizeof(request.data) + 1]{};
    if (operation == st_control::Open || operation == st_control::Remove ||
        operation == st_control::Mkdir || operation == st_control::Rename ||
        (operation == st_control::List && request.size != 0)) {
        if (request.size == 0 || request.size > sizeof(request.data)) {
            reply.status = STATUS_BAD_ARGS;
            return;
        }
        ::memcpy(path, request.data, request.size);
        for (size_t i = 0; i < request.size; ++i)
            if (path[i] == '\0' && operation != st_control::Rename) {
                reply.status = STATUS_BAD_ARGS;
                return;
            }
    }
    if (operation == st_control::Open) {
        constexpr uint64_t allowed =
            st_read | st_write | st_create | st_truncate | st_exclusive | st_append;
        if (request.value == 0 || (request.value & ~allowed) != 0 ||
            ((request.value & (st_create | st_truncate | st_exclusive | st_append)) != 0 &&
             (request.value & st_write) == 0)) {
            reply.status = STATUS_BAD_ARGS;
            return;
        }
        const char* name = boot_name(path);
        const bool on_boot = name != nullptr || !writable_root;
        if ((request.value & st_write) != 0 && !writable) {
            reply.status = STATUS_DENIED;
            return;
        }
        if (on_boot && (request.value != st_read)) {
            reply.status = STATUS_DENIED;
            return;
        }
        if (on_boot) {
            name = name != nullptr ? name : path;
            if (*name == '\0' || ::strlen(name) > sizeof(request.data)) {
                reply.status = STATUS_BAD_ARGS;
                return;
            }
        }
        cap_owner object;
        uint64_t length{}, generation{};
        reply.status =
            on_boot ? fs_open_object(boot_files, name, ::strlen(name), object, length, generation)
                    : st_open(data, path, request.value, object, length, generation);
        if (reply.status != STATUS_OK)
            return;
        constexpr auto rights = RIGHT_SEND | RIGHT_RECEIVE | RIGHT_CLOSE | RIGHT_DESTROY;
        if (!response.offer(std::move(object), rights)) {
            reply.status = STATUS_INTERNAL;
            return;
        }
        reply.value = generation;
        reply.size = sizeof(length);
        ::memcpy(reply.data, &length, sizeof(length));
    } else if (operation == st_control::List) {
        const char* name = request.size == 0 ? "/" : path;
        const char* tail = boot_name(name);
        const bool boot_dir = tail != nullptr || !writable_root;
        if (boot_dir && tail != nullptr && *tail != '\0') {
            reply.status = STATUS_NOT_FOUND;
            return;
        }
        const bool root = (std::string_view{name} == "/") || (std::string_view{name} == "");
        if (root && request.value == 0) {
            ::memcpy(reply.data, "boot", 4);
            reply.size = 4;
            reply.value = UINT64_MAX;
            return;
        }
        const uint64_t cursor = root && request.value == UINT64_MAX ? 0 : request.value;
        ch_msg entry{.value = cursor};
        if (boot_dir)
            reply.status = fs_list(boot_files, entry);
        else
            do {
                reply.status = st_list(data, entry, name);
                if (reply.status != STATUS_OK || !root)
                    break;
                size_t start{};
                reply.size = 0;
                while (start < entry.size) {
                    size_t end = start;
                    while (end < entry.size && entry.data[end] != '\0')
                        ++end;
                    if (end - start != 4 || entry.data[start] != 'b' ||
                        entry.data[start + 1] != 'o' || entry.data[start + 2] != 'o' ||
                        entry.data[start + 3] != 't') {
                        if (reply.size != 0)
                            reply.data[reply.size++] = '\0';
                        ::memcpy(reply.data + reply.size, entry.data + start, end - start);
                        reply.size += end - start;
                    }
                    start = end + 1;
                }
                if (reply.size != 0 || entry.value == 0)
                    break;
                entry = {.value = entry.value};
            } while (true);
        if (reply.status == STATUS_OK) {
            if (boot_dir && entry.size != 0) {
                reply.size = entry.size;
                ::memcpy(reply.data, entry.data, reply.size);
                if (reply.data[reply.size - 1] != '\n') {
                    reply.status = STATUS_PEER_FAULT;
                    return;
                }
                for (size_t i = 0; i < reply.size; ++i)
                    if (reply.data[i] == '\n')
                        reply.data[i] = '\0';
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
    } else if (operation == st_control::Remove || operation == st_control::Mkdir) {
        if (!writable || !writable_root || boot_name(path) != nullptr) {
            reply.status = STATUS_DENIED;
            return;
        }
        reply.status =
            operation == st_control::Remove ? st_remove(data, path) : st_mkdir(data, path);
    } else if (operation == st_control::Rename) {
        const size_t first = ::strlen(path);
        if (first == 0 || first + 1 >= request.size || request.data[first] != '\0') {
            reply.status = STATUS_BAD_ARGS;
            return;
        }
        const char* target = path + first + 1;
        if (*target == '\0' || ::strlen(target) != request.size - first - 1) {
            reply.status = STATUS_BAD_ARGS;
            return;
        }
        reply.status = !writable || !writable_root || boot_name(path) != nullptr ||
                               boot_name(target) != nullptr
                           ? STATUS_DENIED
                           : st_rename(data, path, target);
    } else if (operation == st_control::Sync) {
        if (request.size || request.value)
            reply.status = STATUS_BAD_ARGS;
        else if (!writable)
            reply.status = STATUS_DENIED;
        else if (writable_root)
            reply.status = st_sync(data);
    } else if (operation == st_control::DeviceId || operation == st_control::VolumeId) {
        if (request.size != 0 || request.value != 0) {
            reply.status = STATUS_BAD_ARGS;
            return;
        }
        if (!writable_root) {
            reply.status = STATUS_NOT_FOUND;
            return;
        }
        if (operation == st_control::DeviceId) {
            uint8_t id[20]{};
            reply.status = st_device_id(data, id);
            if (reply.status == STATUS_OK) {
                reply.size = sizeof(id);
                ::memcpy(reply.data, id, reply.size);
            }
        } else {
            uint8_t id[st_volume_id_size]{};
            reply.status = st_volume_id(data, id);
            if (reply.status == STATUS_OK) {
                reply.size = sizeof(id);
                ::memcpy(reply.data, id, reply.size);
            }
        }
    } else
        reply.status = STATUS_INVALID_OP;
}

int main() {

    const auto& info = rt_startup();
    events = info.events;
    uint64_t value{};
    if (const auto rc = boot_files.connect(info.selector(fs_service), info.pool,
                                           info.cspace, info.events,
                                           info.vspace, 0x70000000, value);
        rc != STATUS_OK)
        sys::exit(rc);
    writable_root = info.selector(st_service) != 0;
    if (writable_root)
        if (const auto rc = data.open(info.selector(st_service), info.pool,
                                      info.cspace, events);
            rc != STATUS_OK)
            sys::exit(rc);
    if (const auto rc = directory.open(info.selector(vfs_service), events); rc != STATUS_OK)
        sys::exit(rc);
    if (const auto rc = sys::notification_signal(info.selector(sv_ready)).status; rc != STATUS_OK)
        sys::exit(rc);
    if (const auto rc = sys::notification_signal(info.selector(sv_wake)).status;
        rc != STATUS_OK)
        sys::exit(rc);
    for (;;) {
        ch_packet packet;
        const auto status = directory.receive(packet);
        if (status == STATUS_WOULD_BLOCK) {
            if (const auto rc = directory.arm().status; rc != STATUS_OK)
                sys::exit(rc);
            if (const auto rc = sys::notification_wait(events).status; rc != STATUS_OK)
                sys::exit(rc);
            continue;
        }
        if (status == STATUS_BAD_ARGS || status == STATUS_TRANSFER_FAILED)
            continue;
        if (status != STATUS_OK) sys::exit(status);
        if (packet.count != 1)
            continue;
        ch_reply reply;
        reply.message = {.operation = packet.message.operation, .id = packet.message.id};
        if (packet.badge != st_read_directory && packet.badge != st_write_directory)
            reply.message.status = STATUS_DENIED;
        else
            control(packet.message, reply, packet.badge == st_write_directory);
        reply.respond(packet.capabilities[0].selector());
    }
}
