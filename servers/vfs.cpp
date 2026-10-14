#include <protocols/msg.hpp>
#include <protocols/file.hpp>
#include <protocols/service.hpp>
#include <memory>
#include <string_view>
#include <new>

// Resolve within the granted namespace root, before selecting a mount.
static auto canonical(std::span<char> path) -> size_t {
    std::array<size_t, sizeof(msg_frame::data)> starts{};
    size_t count{}, size{}, cursor{};
    while (cursor < path.size()) {
        const auto begin = cursor;
        while (cursor < path.size() && path[cursor] != '/') ++cursor;
        const std::string_view part{path.data() + begin, cursor - begin};
        if (part == "..") {
            if (count) size = starts[--count];
        } else if (!part.empty() && part != ".") {
            starts[count++] = size;
            if (size) path[size++] = '/';
            ::memmove(path.data() + size, part.data(), part.size());
            size += part.size();
        }
        ++cursor;
    }
    return size;
}
static auto boot_tail(std::string_view path) -> std::optional<std::string_view> {
    if (path == "boot")
        return std::string_view{};
    if (path.starts_with("boot/"))
        return path.substr(5);
    return {};
}
static void respond(cap_t reply, const msg_frame& request, status_t status) {
    msg_reply result;
    result.message = {.operation = request.operation, .status = status};
    result.respond(reply);
}
struct vf_route {
    const rt_req* provider{};
    bool filter{};
    status_t status{};
};
static auto route(msg_frame& req, word_t perms) -> vf_route {
    const bool writable = perms & fl_write;
    if (const auto status = rt_admit(req.deadline); status != STATUS_OK)
        return {.status = status};
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
        return {&st_service};
    }
    if (has_path) {
        const auto cut = path.find('\0');
        if (op == st_control::Rename && (cut == path.npos || !cut || cut + 1 == path.size() ||
                                         path.substr(cut + 1).find('\0') != path.npos))
            return {.status = STATUS_BAD_ARGS};
        const auto first = canonical(std::span{req.data, cut == path.npos ? req.size : cut});
        if (op == st_control::Rename) {
            auto* second_path = req.data + cut + 1;
            const auto second = canonical(std::span{second_path, req.size - cut - 1});
            if (!first || !second)
                return {.status = STATUS_BAD_ARGS};
            req.data[first] = '\0';
            ::memmove(req.data + first + 1, second_path, second);
            req.size = first + 1 + second;
        } else
            req.size = first;
        path = {req.data, req.size};
    }
    const auto tail = boot_tail(path.substr(0, path.find('\0')));
    const bool on_boot = tail.has_value();
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
        if ((req.value & VM_EXECUTE) && !(perms & fl_exec))
            return {.status = STATUS_DENIED};
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
        return {on_boot ? &fs_service : &st_service, root && !on_boot};
    } else
        return {.status = STATUS_INVALID_OP};
    if (on_boot && tail) {
        ::memmove(req.data, tail->data(), tail->size());
        req.size = tail->size();
    }
    return {on_boot ? &fs_service : &st_service};
}
// A logical forward owns its provider until admission or final list reply.
// Never look up a replacement while waiting or skipping covered entries.
struct vf_request {
    std::unique_ptr<vf_request> next;
    msg_frame request;
    cap_owner caller, provider;
    cap_t target{};
    word_t writable{}, sequence{};
    ch_pair bridge;
    msg_rx receiver;
    cap_owner sender;

    ~vf_request() {
        if (writable) {
            const auto status = sys::channel_unbind(target, writable).status;
            if (status != STATUS_OK && status != STATUS_BUSY) sys::exit(status);
        }
    }
    auto prepare(const rt_env& env) -> status_t {
        auto status = receiver.close();
        if (status == STATUS_OK) status = bridge.open(env.pool, 1, 0, 1);
        if (status == STATUS_OK) status = receiver.open(bridge.ends[0].selector(), env.events);
        if (status != STATUS_OK) return status;
        const auto minted =
            sys::channel_mint(bridge.ends[1].selector(), env.cspace, 1, RIGHT_SEND | RIGHT_CLOSE);
        sender = cap_owner{minted.value};
        if (minted.status == STATUS_OK) bridge.ends[1] = {};
        return minted.status;
    }
    auto advance(const rt_env& env) -> bool {
        const auto fail = [&](status_t status) {
            respond(caller.selector(), request, status);
            return true;
        };
        for (;;) {
            if (const auto status = rt_admit(request.deadline); status != STATUS_OK)
                return fail(status);
            if (!receiver || sender) {
                auto& reply = receiver ? sender : caller;
                const CapXfer transfer{reply.selector(), 0, CAP_MOVE, 0};
                const auto result = msg_send(target, request, false, {&transfer, 1});
                if (result.status == STATUS_WOULD_BLOCK) {
                    if (!writable) {
                        const auto bound = sys::channel_bind(target, env.events, CHANNEL_WRITABLE);
                        if (bound.status != STATUS_OK) return fail(bound.status);
                        writable = bound.value;
                    }
                    const auto armed = sys::channel_arm(target, writable, sequence);
                    if (armed.status != STATUS_OK) return fail(armed.status);
                    sequence = armed.value;
                    return false;
                }
                if (result.status != STATUS_OK) return fail(result.status);
                sequence = result.value;
                (void)reply.release();
                if (!receiver) return true;
            }
            msg_packet packet;
            const auto status = receiver.receive(packet);
            if (status == STATUS_WOULD_BLOCK) {
                const auto armed = receiver.arm().status;
                return armed == STATUS_OK ? false : fail(armed);
            }
            if (status != STATUS_OK) return fail(status);
            if (packet.count || packet.message.operation != request.operation)
                return fail(STATUS_PEER_FAULT);
            auto& reply = packet.message;
            if (reply.status == STATUS_OK) {
                size_t start{}, size{};
                while (start < reply.size) {
                    auto end = start;
                    while (end < reply.size && reply.data[end]) ++end;
                    if (std::string_view{reply.data + start, end - start} != "boot") {
                        if (size) reply.data[size++] = '\0';
                        ::memmove(reply.data + size, reply.data + start, end - start);
                        size += end - start;
                    }
                    start = end + 1;
                }
                reply.size = size;
                if (!size && reply.value) {
                    request.value = reply.value;
                    if (const auto prepared = prepare(env); prepared != STATUS_OK)
                        return fail(prepared);
                    continue;
                }
            }
            msg_reply response;
            response.message = reply;
            response.respond(caller.selector());
            return true;
        }
    }
};
int main() {
    const auto& env = rt_startup();
    const auto boot = env.selector(fs_service);
    rpc_client registry;
    if (const auto status = registry.open(env.selector(sv_service), env.pool, env.cspace, env.events);
        status != STATUS_OK) sys::exit(status);
    msg_rx incoming{env.selector(vfs_service), env.events};
    if (const auto rc = sys::notification_signal(env.selector(sv_ready)).status; rc != STATUS_OK)
        sys::exit(rc);
    if (const auto rc = sys::notification_signal(env.selector(sv_wake)).status; rc != STATUS_OK)
        sys::exit(rc);
    std::unique_ptr<vf_request> requests;
    for (;;) {
        bool again{};
        for (auto* link = &requests; *link;) {
            if ((*link)->advance(env)) {
                auto retired = std::move(*link);
                *link = std::move(retired->next);
                again = true;
            } else link = &(*link)->next;
        }
        msg_packet packet;
        const auto received = incoming.receive(packet);
        if (received == STATUS_OK && packet.count == 1) {
            again = true;
            auto request = packet.message;
            auto caller = std::move(packet.capabilities[0]);
            const auto plan =
                ((packet.badge & fl_read) && !(packet.badge & ~(fl_read | fl_write | fl_exec)))
                    ? route(request, packet.badge)
                    : vf_route{.status = STATUS_DENIED};
            cap_owner provider;
            auto status = plan.status;
            if (status == STATUS_OK && plan.provider == &st_service) {
                auto found = sv_lookup(registry, st_service, request.deadline);
                if (found) provider = std::move(*found);
                else status = found.error();
                // No data import is different from a configured, offline mount.
                if (status == STATUS_NOT_FOUND &&
                    ((request.operation == static_cast<uint64_t>(st_control::List) && !request.size) ||
                     request.operation == static_cast<uint64_t>(st_control::Sync)))
                    status = STATUS_OK;
            }
            if (status != STATUS_OK) respond(caller.selector(), request, status);
            else if (!plan.provider || (plan.provider == &st_service && !provider)) {
                msg_reply reply;
                reply.message.operation = request.operation;
                if (!plan.provider && request.operation == static_cast<uint64_t>(st_control::List)) {
                    reply.message.size = 4;
                    reply.message.value = UINT64_MAX;
                    ::memcpy(reply.message.data, "boot", 4);
                }
                reply.respond(caller.selector());
            } else {
                auto work = std::unique_ptr<vf_request>{new (std::nothrow) vf_request};
                if (!work) respond(caller.selector(), request, STATUS_NO_MEMORY);
                else {
                    work->request = request;
                    work->caller = std::move(caller);
                    work->provider = std::move(provider);
                    work->target = work->provider ? work->provider.selector() : boot;
                    status = plan.filter ? work->prepare(env) : STATUS_OK;
                    if (status != STATUS_OK) respond(work->caller.selector(), request, status);
                    else {
                        work->next = std::move(requests);
                        requests = std::move(work);
                    }
                }
            }
        } else if (received != STATUS_OK && received != STATUS_WOULD_BLOCK &&
                   received != STATUS_BAD_ARGS && received != STATUS_TRANSFER_FAILED)
            sys::exit(received);
        if (const auto status = incoming.arm().status; status != STATUS_OK) sys::exit(status);
        if (!again) {
            uint64_t deadline{};
            for (auto* work = requests.get(); work; work = work->next.get())
                if (const auto limit = work->request.deadline;
                    limit && (!deadline || limit < deadline)) deadline = limit;
            const auto status = sys::notification_wait(env.events, deadline).status;
            if (status != STATUS_OK && status != STATUS_TIMED_OUT) sys::exit(status);
        }
    }
}
