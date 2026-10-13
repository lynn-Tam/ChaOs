#include <protocols/file.hpp>
#include <protocols/service.hpp>
#include <deque>
#include <memory>
#include <string_view>
#include <string>
#include <vector>

struct vf_provider {
    cap_t channel{};
    word_t writable{}, sequence{};
    auto open(cap_t cap, cap_t events) -> status_t {
        if (!cap)
            return STATUS_OK;
        const auto bound = sys::channel_bind(cap, events, CHANNEL_WRITABLE);
        if (bound.status == STATUS_OK) {
            channel = cap;
            writable = bound.value;
        }
        return bound.status;
    }
    auto send(const ch_msg& request, cap_owner& reply) -> status_t {
        const CapXfer transfer{reply.selector(), 0, CAP_MOVE, 0};
        const auto sent = ch_send(channel, request, false, {&transfer, 1});
        if (sent.status == STATUS_OK) {
            sequence = sent.value;
            (void) reply.release();
        }
        if (sent.status == STATUS_WOULD_BLOCK) {
            const auto armed = sys::channel_arm(channel, writable, sequence);
            if (armed.status != STATUS_OK)
                return armed.status;
            sequence = armed.value;
        }
        return sent.status;
    }
};
struct vf_pending {
    vf_provider* target;
    ch_msg request;
    cap_owner reply;
};
// Namespace enumeration alone needs to inspect a provider reply, to hide the
// underlying directory entry covered by the boot mount. Ordinary replies bypass VFS.
struct vf_list {
    ch_msg request;
    cap_owner caller;
    ch_pair bridge;
    ch_port receiver;
    cap_owner sender;
    std::optional<ch_msg> result;
    auto prepare(cap_t pool, cap_t space, cap_t events) -> status_t {
        auto status = receiver.close();
        if (status == STATUS_OK)
            status = bridge.open(pool, 1, 0, 1);
        if (status == STATUS_OK)
            status = receiver.open(bridge.ends[0].selector(), events);
        if (status != STATUS_OK)
            return status;
        const auto minted =
            sys::channel_mint(bridge.ends[1].selector(), space, 1, RIGHT_SEND | RIGHT_CLOSE);
        sender = cap_owner{minted.value};
        if (minted.status == STATUS_OK)
            bridge.ends[1] = {};
        return minted.status;
    }
};
static vf_provider boot, data;
// Resolve within the granted namespace root, before selecting a mount.
static auto canonical(std::string_view path) -> std::string {
    std::vector<std::string_view> parts;
    while (!path.empty()) {
        const auto end = path.find('/');
        const auto part = path.substr(0, end);
        if (part == "..") {
            if (!parts.empty())
                parts.pop_back();
        } else if (!part.empty() && part != ".")
            parts.push_back(part);
        if (end == path.npos)
            break;
        path.remove_prefix(end + 1);
    }
    std::string result;
    for (auto part : parts) {
        if (!result.empty())
            result += '/';
        result += part;
    }
    return result;
}
static auto boot_tail(std::string_view path) -> std::optional<std::string_view> {
    if (path == "boot")
        return std::string_view{};
    if (path.starts_with("boot/"))
        return path.substr(5);
    return {};
}
static void respond(cap_t reply, const ch_msg& request, status_t status) {
    ch_reply result;
    result.message = {.operation = request.operation, .id = request.id, .status = status};
    result.respond(reply);
}
struct vf_route {
    vf_provider* provider{};
    bool filter{};
    status_t status{};
};
static auto route(ch_msg& req, bool writable) -> vf_route {
    const auto op = static_cast<st_control>(req.operation);
    const auto has_path = op == st_control::Open || op == st_control::Map ||
                          op == st_control::Remove || op == st_control::Mkdir ||
                          op == st_control::Rename || (op == st_control::List && req.size);
    std::string_view path{req.data, req.size};
    if (has_path && (path.empty() || (op != st_control::Rename && path.find('\0') != path.npos)))
        return {.status = STATUS_BAD_ARGS};
    if (op == st_control::Sync || op == st_control::DeviceId || op == st_control::VolumeId) {
        if (req.size || req.value)
            return {.status = STATUS_BAD_ARGS};
        if (op == st_control::Sync && !writable)
            return {.status = STATUS_DENIED};
        return data.channel
                   ? vf_route{&data}
                   : vf_route{.status = op == st_control::Sync ? STATUS_OK : STATUS_NOT_FOUND};
    }
    if (has_path) {
        const auto cut = path.find('\0');
        if (op == st_control::Rename && (cut == path.npos || !cut || cut + 1 == path.size() ||
                                         path.substr(cut + 1).find('\0') != path.npos))
            return {.status = STATUS_BAD_ARGS};
        const auto first = canonical(path.substr(0, cut));
        const auto second =
            op == st_control::Rename ? canonical(path.substr(cut + 1)) : std::string{};
        if (op == st_control::Rename && (first.empty() || second.empty()))
            return {.status = STATUS_BAD_ARGS};
        req.size = first.size();
        ::memcpy(req.data, first.data(), first.size());
        if (op == st_control::Rename) {
            req.data[req.size++] = '\0';
            ::memcpy(req.data + req.size, second.data(), second.size());
            req.size += second.size();
        }
        path = {req.data, req.size};
    }
    const auto tail = boot_tail(path.substr(0, path.find('\0')));
    const bool on_boot = tail.has_value() || !data.channel;
    if (op == st_control::Open) {
        constexpr auto allowed =
            st_read | st_write | st_create | st_truncate | st_exclusive | st_append;
        if (!req.value || (req.value & ~allowed) ||
            ((req.value & ~(st_read | st_write)) && !(req.value & st_write)))
            return {.status = STATUS_BAD_ARGS};
        if ((req.value & st_write) && (!writable || on_boot))
            return {.status = STATUS_DENIED};
    } else if (op == st_control::Map) {
        if (req.value != VM_READ && req.value != (VM_READ | VM_EXECUTE))
            return {.status = STATUS_BAD_ARGS};
    } else if (op == st_control::Remove || op == st_control::Mkdir) {
        if (!writable || on_boot)
            return {.status = STATUS_DENIED};
    } else if (op == st_control::Rename) {
        const auto cut = path.find('\0');
        if (cut == path.npos || !cut || cut + 1 == path.size() ||
            path.substr(cut + 1).find('\0') != path.npos)
            return {.status = STATUS_BAD_ARGS};
        if (!writable || on_boot || boot_tail(path.substr(cut + 1)))
            return {.status = STATUS_DENIED};
    } else if (op == st_control::List) {
        if (on_boot && tail && !tail->empty())
            return {.status = STATUS_NOT_FOUND};
        const bool root = path.empty() || path == "/";
        if (root && req.value == 0)
            return {};
        if (root && req.value == UINT64_MAX)
            req.value = 0;
        if (on_boot)
            req.size = 0;
        return {on_boot ? &boot : &data, root && !on_boot};
    } else
        return {.status = STATUS_INVALID_OP};
    if (on_boot && tail) {
        ::memmove(req.data, tail->data(), tail->size());
        req.size = tail->size();
    }
    return {on_boot ? &boot : &data};
}
int main() {
    const auto& env = rt_startup();
    for (auto [provider, cap] : std::array{std::pair{&boot, env.selector(fs_service)},
                                           std::pair{&data, env.selector(st_service)}})
        if (const auto rc = provider->open(cap, env.events); rc != STATUS_OK)
            sys::exit(rc);
    ch_port incoming{env.selector(vfs_service), env.events};
    if (const auto rc = sys::notification_signal(env.selector(sv_ready)).status; rc != STATUS_OK)
        sys::exit(rc);
    if (const auto rc = sys::notification_signal(env.selector(sv_wake)).status; rc != STATUS_OK)
        sys::exit(rc);
    std::optional<vf_pending> pending;
    std::deque<std::unique_ptr<vf_list>> lists;
    for (;;) {
        bool again{};
        if (pending) {
            const auto rc = pending->target->send(pending->request, pending->reply);
            if (rc != STATUS_WOULD_BLOCK) {
                if (rc != STATUS_OK)
                    respond(pending->reply.selector(), pending->request, rc);
                pending.reset();
                again = true;
            }
        }
        for (auto i = lists.begin(); i != lists.end();) {
            auto& list = **i;
            auto rc = STATUS_OK;
            if (!list.result) {
                ch_packet packet;
                rc = list.receiver.receive(packet);
                if (rc == STATUS_WOULD_BLOCK) {
                    if (const auto armed = list.receiver.arm().status; armed != STATUS_OK)
                        sys::exit(armed);
                    ++i;
                    continue;
                }
                if (rc == STATUS_OK && (packet.count || packet.message.id != list.request.id ||
                                        packet.message.operation != list.request.operation))
                    rc = STATUS_PEER_FAULT;
                list.result = packet.message;
            }
            auto& reply = *list.result;
            if (rc == STATUS_OK && reply.status == STATUS_OK) {
                size_t start{}, size{};
                while (start < reply.size) {
                    auto end = start;
                    while (end < reply.size && reply.data[end])
                        ++end;
                    if (std::string_view{reply.data + start, end - start} != "boot") {
                        if (size)
                            reply.data[size++] = '\0';
                        ::memmove(reply.data + size, reply.data + start, end - start);
                        size += end - start;
                    }
                    start = end + 1;
                }
                reply.size = size;
                if (!size && reply.value && !pending) {
                    list.request.value = reply.value;
                    rc = list.prepare(env.pool, env.cspace, env.events);
                    if (rc == STATUS_OK) {
                        pending.emplace(&data, list.request, std::move(list.sender));
                        list.result.reset();
                        again = true;
                        ++i;
                        continue;
                    }
                } else if (!size && reply.value && pending) {
                    ++i;
                    continue;
                }
            }
            if (rc != STATUS_OK)
                respond(list.caller.selector(), list.request, rc);
            else {
                ch_reply result;
                result.message = reply;
                result.respond(list.caller.selector());
            }
            i = lists.erase(i);
            again = true;
        }
        if (!pending) {
            ch_packet packet;
            const auto rc = incoming.receive(packet);
            if (rc == STATUS_OK && packet.count == 1) {
                again = true;
                auto request = packet.message;
                const auto plan =
                    (packet.badge == st_read_directory || packet.badge == st_write_directory)
                        ? route(request, packet.badge == st_write_directory)
                        : vf_route{.status = STATUS_DENIED};
                auto caller = std::move(packet.capabilities[0]);
                if (plan.status != STATUS_OK)
                    respond(caller.selector(), request, plan.status);
                else if (!plan.provider) {
                    ch_reply reply;
                    reply.message = {.operation = request.operation, .id = request.id};
                    if (request.operation == static_cast<uint64_t>(st_control::List)) {
                        reply.message.size = 4;
                        reply.message.value = UINT64_MAX;
                        ::memcpy(reply.message.data, "boot", 4);
                    }
                    reply.respond(caller.selector());
                } else if (plan.filter) {
                    auto list = std::make_unique<vf_list>();
                    list->request = request;
                    list->caller = std::move(caller);
                    const auto status = list->prepare(env.pool, env.cspace, env.events);
                    if (status != STATUS_OK)
                        respond(list->caller.selector(), request, status);
                    else {
                        pending.emplace(plan.provider, request, std::move(list->sender));
                        lists.push_back(std::move(list));
                    }
                } else
                    pending.emplace(plan.provider, request, std::move(caller));
            } else if (rc != STATUS_OK && rc != STATUS_WOULD_BLOCK && rc != STATUS_BAD_ARGS &&
                       rc != STATUS_TRANSFER_FAILED)
                sys::exit(rc);
        }
        if (const auto rc = incoming.arm().status; rc != STATUS_OK)
            sys::exit(rc);
        if (!again)
            if (const auto rc = sys::notification_wait(env.events).status; rc != STATUS_OK)
                sys::exit(rc);
    }
}
