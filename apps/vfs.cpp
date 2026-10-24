#include <io.hpp>
#include <libk/path.hpp>
#include <ubus/ubus.hpp>
#include <map>
#include <set>
#include <deque>


struct vf_binding {
    ubus_call lookup;
    ubus_conn connection;
    uint64_t generation{};
    unsigned phase{};
    status_t status{STATUS_PENDING};
};
struct vf_mount {
    std::string service;
    bool read_only{};
    std::shared_ptr<vf_binding> binding;
};
struct vf_target {
    std::shared_ptr<vf_mount> mount;
    std::shared_ptr<vf_binding> binding;
    ubus_msg input, output;
    ubus_call call;
    status_t status{STATUS_PENDING};
    bool sent{};
};
struct vf_job {
    ubus_request request;
    ev_watch canceled;
    std::string method;
    std::deque<vf_target> targets;
    std::set<std::string> covered;
    uint64_t epoch{};
};
static std::map<std::string, std::shared_ptr<vf_mount>, std::less<>> mounts;
static std::vector<std::unique_ptr<vf_job>> jobs;
static uint64_t epoch{1}, next_binding{};
static ubus_conn bus;
static ubus_object directory{"io.files"};

static auto route(std::string_view path) -> decltype(mounts.begin()) {
    auto result = mounts.end();
    for (auto mount = mounts.begin(); mount != mounts.end(); ++mount) {
        const auto& prefix = mount->first;
        if ((prefix == "/" || path == prefix || (path.starts_with(prefix) && path.size() > prefix.size() && path[prefix.size()] == '/'))
            && (result == mounts.end() || prefix.size() > result->first.size())) result = mount;
    }
    return result;
}
static auto relative(std::string_view path, std::string_view prefix) -> std::string_view {
    if (prefix == "/") return path;
    return path.size() == prefix.size() ? std::string_view{"/"} : path.substr(prefix.size());
}
// Jobs pin the selected mount and binding across Unmount/replacement. A failed
// attempt only invalidates future resolution; an accepted call is never replayed.
static auto target(vf_job& job, const std::shared_ptr<vf_mount>& mount, ubus_msg input,
                   uint64_t generation = 0) -> status_t {
    auto& binding = mount->binding;
    if (binding && binding->status == STATUS_OK && binding->connection.state() != STATUS_OK) {
        binding.reset(); ++epoch;
    }
    if (generation && (!binding || binding->generation != generation)) return STATUS_STALE;
    if (binding && binding->status != STATUS_OK && binding->status != STATUS_PENDING) binding.reset();
    if (!binding) { binding = std::make_shared<vf_binding>(); binding->generation = ++next_binding; }
    auto& item = job.targets.emplace_back(); item.mount = mount; item.binding = binding; item.input = std::move(input);
    return STATUS_OK;
}
static auto enqueue(std::string_view method, ubus_request& request, ubus_msg input,
                    const std::shared_ptr<vf_mount>& mount, uint64_t generation = 0,
                    std::set<std::string> covered = {}) -> status_t {
    if (jobs.size() >= 32) return STATUS_BUSY;
    auto& job = *jobs.emplace_back(std::make_unique<vf_job>()); job.method = method; job.covered = std::move(covered);
    const auto status = target(job, mount, std::move(input), generation);
    if (status != STATUS_OK) { jobs.pop_back(); return status; }
    const auto watched = job.canceled.open(rt_events(), request.reply.selector(), SOURCE_TERMINAL, 0, 0, SOURCE_WRITE);
    if (watched != STATUS_OK) { jobs.pop_back(); return watched; }
    job.epoch = epoch; job.request = std::move(request); return STATUS_PENDING;
}
static auto advance(vf_target& item, std::string_view method, word_t deadline) -> bool {
    if (item.status != STATUS_PENDING) return false;
    auto& binding = *item.binding;
    if (binding.status == STATUS_PENDING) {
        auto status = STATUS_OK;
        if (!binding.phase) {
            ubus_msg query; query.deadline = deadline; query.tag = *bus.token("Get"); query.data.put("name", item.mount->service);
            status = binding.lookup.begin(bus.endpoint(), std::move(query));
            if (status == STATUS_OK) binding.phase = 1;
        } else if (binding.phase == 1) {
            if (!binding.lookup.pending()) return false;
            ubus_msg reply; status = binding.lookup.poll(reply);
            if (status == STATUS_OK) {
                status = reply.status;
                if (status == STATUS_OK) status = binding.connection.begin_bind(reply.capability(reply.data["connector"]), "io.files", 1, 1, 0, deadline);
                if (status == STATUS_OK) binding.phase = 2;
            }
        } else {
            if (!binding.connection.pending()) return false;
            status = binding.connection.poll_bind();
            if (status == STATUS_OK) binding.status = STATUS_OK;
        }
        if (status != STATUS_OK && status != STATUS_WOULD_BLOCK) binding.status = status;
        if (binding.status == STATUS_PENDING) return status != STATUS_WOULD_BLOCK;
    }
    if (binding.status != STATUS_OK) { item.status = binding.status; return true; }
    if (!item.sent) {
        const auto token = binding.connection.token(method);
        if (!token) { item.status = STATUS_INVALID_OP; return true; }
        item.input.tag = *token;
        const auto status = item.call.begin(binding.connection.endpoint(), std::move(item.input));
        if (status != STATUS_OK) { item.status = status; return true; }
        item.sent = true; return true;
    }
    if (!item.call.pending()) return false;
    auto status = item.call.poll(item.output);
    if (status == STATUS_WOULD_BLOCK) return false;
    item.status = status == STATUS_OK ? item.output.status : status;
    if (status != STATUS_OK && status != STATUS_TIMED_OUT && status != STATUS_CANCELED
        && item.mount->binding == item.binding) { item.mount->binding.reset(); ++epoch; }
    return true;
}
static auto complete(vf_job& job, ubus_msg& response) -> status_t {
    for (const auto& item : job.targets) if (item.status != STATUS_OK) return item.status;
    if (job.method == "Sync") return STATUS_OK;
    auto& item = job.targets.front(); auto& result = item.output;
    if (job.method == "List") {
        if (job.epoch != epoch) return STATUS_STALE;
        if (result.data["items"].type() != ubus_type::Array) return STATUS_PEER_FAULT;
        ubus_data items{ubus_type::Array};
        for (const auto entry : result.data["items"].values()) {
            const auto name = entry["name"].as<std::string_view>(); if (!name) return STATUS_PEER_FAULT;
            if (!job.covered.contains(std::string{*name})) items.push(entry);
        }
        response.data.put("epoch", epoch).put("items", items);
        if (const auto backend = result.data["cursor"]) {
            if (backend.type() != ubus_type::Record) return STATUS_PEER_FAULT;
            ubus_data next; next.put("epoch", epoch).put("generation", item.binding->generation).put("backend", backend);
            response.data.put("cursor", next);
        }
        return STATUS_OK;
    }
    response.data = std::move(result.data);
    if (job.method == "Map" || job.method == "Open" || job.method == "Document") {
        auto cap = result.take(response.data[job.method == "Map" ? "buffer" : "handle"]);
        if (!cap || result.count != 1) return STATUS_PEER_FAULT;
        const auto index = job.method == "Map" ? response.offer(std::move(cap), RIGHT_MAP | RIGHT_DUPLICATE | RIGHT_DELEGATE)
            : response.give(std::move(cap));
        if (!index) return index.error();
    }
    return STATUS_OK;
}
static auto control(std::string_view method, ubus_request& request, ubus_msg&) -> status_t {
    if (method == "Sync") {
        if (!(request.facet() & io_modify)) return STATUS_DENIED;
        if (jobs.size() >= 32) return STATUS_BUSY;
        auto& job = *jobs.emplace_back(std::make_unique<vf_job>()); job.method = method;
        for (const auto& [path, mount] : mounts) if (!mount->read_only) {
            ubus_msg input; input.deadline = request.deadline;
            const auto status = target(job, mount, std::move(input));
            if (status != STATUS_OK) { jobs.pop_back(); return status; }
        }
        const auto watched = job.canceled.open(rt_events(), request.reply.selector(), SOURCE_TERMINAL, 0, 0, SOURCE_WRITE);
        if (watched != STATUS_OK) { jobs.pop_back(); return watched; }
        job.request = std::move(request); return STATUS_PENDING;
    }
    const auto path = libk::path_normal(request.data["path"].as<std::string_view>().value_or("/"));
    if (path.empty()) return STATUS_BAD_ARGS;
    auto mount = route(path);
    if (mount == mounts.end()) return method == "Sync" ? STATUS_OK : STATUS_NOT_FOUND;
    const bool mutate = method == "Open" ? (*request.data["flags"].as<uint64_t>() & io_write) != 0
        : method == "Remove" || method == "Mkdir" || method == "Rename" || method == "Sync";
    if (mutate && (!(request.facet() & io_modify) || mount->second->read_only)) return STATUS_DENIED;
    if (method == "Map") {
        const auto perms = *request.data["perms"].as<word_t>();
        if (perms != VM_READ && perms != (VM_READ | VM_EXECUTE)) return STATUS_BAD_ARGS;
        if ((perms & VM_EXECUTE) && !(request.facet() & io_execute)) return STATUS_DENIED;
    }
    if (method == "Open") {
        const auto flags = *request.data["flags"].as<uint64_t>();
        if (!(flags & (io_read | io_write)) || flags & ~(io_read | io_write | io_create | io_truncate | io_exclusive | io_append)
            || ((flags & ~(io_read | io_write)) && !(flags & io_write))) return STATUS_BAD_ARGS;
    }
    ubus_msg forwarded; forwarded.deadline = request.deadline;
    if (method == "Rename") {
        const auto target = libk::path_normal(*request.data["to"].as<std::string_view>());
        if (target.empty()) return STATUS_BAD_ARGS;
        if (route(target) != mount) return io_cross_device;
        forwarded.data.put("from", relative(path, mount->first)).put("to", relative(target, mount->first));
    } else if (method != "Sync" && method != "DeviceId" && method != "VolumeId") forwarded.data.put("path", relative(path, mount->first));
    if (method == "Map") forwarded.data.put("perms", *request.data["perms"].as<word_t>());
    if (method == "Open") {
        forwarded.data.put("flags", *request.data["flags"].as<uint64_t>());
    }
    if (method == "Document") forwarded.data.put("writable", bool((request.facet() & io_modify) && !mount->second->read_only));
    if (const auto budget = request.data["budget"]) {
        const auto index = forwarded.attach(request.capability(budget), RIGHT_CREATE | RIGHT_INSPECT | RIGHT_DUPLICATE);
        if (!index) return index.error();
        forwarded.data.cap("budget", *index);
    }
    return enqueue(method, request, std::move(forwarded), mount->second);
}
static auto children(std::string_view path) -> std::set<std::string> {
    std::set<std::string> result;
    const auto prefix = path == "/" ? std::string{"/"} : std::string{path} + "/";
    for (const auto& [name, mount] : mounts) if (name.starts_with(prefix) && name.size() > prefix.size()) {
        const auto tail = std::string_view{name}.substr(prefix.size());
        result.emplace(tail.substr(0, tail.find('/')));
    }
    return result;
}
static auto list(ubus_request& request, ubus_msg& response) -> status_t {
    const auto path = libk::path_normal(*request.data["path"].as<std::string_view>());
    const auto cursor = request.data["cursor"];
    if (cursor && !cursor.data().empty() && cursor["epoch"].as<uint64_t>() != epoch) return STATUS_STALE;
    const auto covered = children(path);
    if (path.empty()) return STATUS_BAD_ARGS;
    auto mount = route(path);
    ubus_data items{ubus_type::Array};
    const bool first = !cursor || cursor.data().empty();
    if (first && (!covered.empty() || mount == mounts.end())) {
        if (covered.empty() && path != "/") return io_not_dir;
        for (const auto& name : covered) { ubus_data item; item.put("name", name).put("type", "dir"); items.push(item); }
        response.data.put("epoch", epoch).put("items", items);
        if (mount != mounts.end()) {
            ubus_data next, backend; next.put("epoch", epoch).put("generation", uint64_t{0}).put("backend", backend);
            response.data.put("cursor", next);
        }
        return STATUS_OK;
    }
    if (mount == mounts.end()) return STATUS_STALE;
    const auto generation = first ? uint64_t{0} : cursor["generation"].as<uint64_t>().value_or(UINT64_MAX);
    ubus_msg forwarded; forwarded.deadline = request.deadline;
    forwarded.data.put("path", relative(path, mount->first));
    if (!first) {
        if (cursor["backend"].type() != ubus_type::Record) return STATUS_BAD_ARGS;
        forwarded.data.put("cursor", cursor["backend"]);
    }
    return enqueue("List", request, std::move(forwarded), mount->second, generation, covered);
}
int main() {
    const auto& env = rt_startup();
    auto status = bus.bind(env.environment);
    if (status != STATUS_OK) return status;
    ubus_msg setup;
    status = bus.invoke("Receive", {}, setup); if (status != STATUS_OK) return status;
    for (const auto config : setup.data["message"]["mounts"].values()) {
        const auto path = config["path"].as<std::string_view>(), service = config["service"].as<std::string_view>();
        const auto read_only = config["read_only"].as<bool>();
        if (!path || !service || !read_only) return STATUS_BAD_ARGS;
        auto mount = std::make_shared<vf_mount>(std::string{*service}, *read_only);
        const auto normalized = libk::path_normal(*path);
        if (normalized.empty()) return STATUS_BAD_ARGS;
        mounts.emplace(normalized, std::move(mount));
    }
    for (const auto result : {
        directory.add("Map", {{"path", ubus_type::String}, {"perms", ubus_type::U64}}, {{"buffer", ubus_type::Buffer}}, [](auto& q, auto& r) { return control("Map", q, r); }, io_lookup),
        directory.add("Document", {{"path", ubus_type::String}, {"budget", ubus_type::Cap, true}}, {{"handle", ubus_type::Cap}}, [](auto& q, auto& r) { return control("Document", q, r); }, io_lookup),
        directory.add("Open", {{"path", ubus_type::String}, {"flags", ubus_type::U64}, {"budget", ubus_type::Cap, true}}, {{"handle", ubus_type::Cap}}, [](auto& q, auto& r) { return control("Open", q, r); }, io_lookup),
        directory.add("List", {{"path", ubus_type::String}, {"cursor", ubus_type::Record, true}}, {{"epoch", ubus_type::U64}, {"items", ubus_type::Array}, {"cursor", ubus_type::Record, true}}, list, io_lookup),
        directory.add("Remove", {{"path", ubus_type::String}}, {}, [](auto& q, auto& r) { return control("Remove", q, r); }, io_modify),
        directory.add("Mkdir", {{"path", ubus_type::String}}, {}, [](auto& q, auto& r) { return control("Mkdir", q, r); }, io_modify),
        directory.add("Rename", {{"from", ubus_type::String}, {"to", ubus_type::String}}, {}, [](auto& q, auto& r) {
            ubus_data input; input.put("path", q.data["from"]).put("to", q.data["to"]); q.data = std::move(input); return control("Rename", q, r);
        }, io_modify),
        directory.add("Sync", {}, {}, [](auto& q, auto& r) { return control("Sync", q, r); }, io_modify),
        directory.add("DeviceId", {}, {{"id", ubus_type::Bytes}}, [](auto& q, auto& r) { return control("DeviceId", q, r); }, io_lookup),
        directory.add("VolumeId", {}, {{"id", ubus_type::Bytes}}, [](auto& q, auto& r) { return control("VolumeId", q, r); }, io_lookup),
        directory.add("Mount", {{"path", ubus_type::String}, {"service", ubus_type::String}, {"read_only", ubus_type::Bool}}, {}, [](auto& q, auto&) {
            const auto path = libk::path_normal(*q.data["path"].template as<std::string_view>());
            if (path.empty()) return STATUS_BAD_ARGS;
            mounts.insert_or_assign(path, std::make_shared<vf_mount>(std::string{*q.data["service"].template as<std::string_view>()}, *q.data["read_only"].template as<bool>()));
            ++epoch; return STATUS_OK;
        }, io_admin),
        directory.add("Unmount", {{"path", ubus_type::String}}, {}, [](auto& q, auto&) {
            if (!mounts.erase(libk::path_normal(*q.data["path"].template as<std::string_view>()))) return STATUS_NOT_FOUND;
            ++epoch; return STATUS_OK;
        }, io_admin)}) if (result != STATUS_OK) return result;
    // At most 32 jobs retain original/forwarded control bodies. Keep that
    // commitment within this service's budget; bulk is a BufferSlice.
    if ((status = directory.open(32, 0, {}, 8 * 1024)) != STATUS_OK) return status;
    for (const auto& [name, rights] : std::array<std::pair<std::string_view, word_t>, 4>{{
        {"files", io_lookup | io_modify}, {"read", io_lookup}, {"exec", io_lookup | io_execute}, {"admin", io_admin}}}) {
        auto connector = directory.connector(rights); if (!connector) return connector.error();
        ubus_msg request, response;
        const auto peer_idx = request.attach(connector->selector(), RIGHT_SEND | RIGHT_DUPLICATE);
        if (!peer_idx) return peer_idx.error();
        request.data.put("name", name).put("interface", "io.files").put("version", uint64_t{1})
            .cap("connector", *peer_idx);
        status = bus.invoke("Publish", std::move(request), response); if (status != STATUS_OK) return status;
    }
    for (;;) {
        bool again{};
        for (size_t n = 0; n < 32; ++n) {
            status = directory.poll(); if (status == STATUS_WOULD_BLOCK) break;
            if (status != STATUS_OK) return status;
            again = true;
        }
        word_t deadline{};
        for (auto job = jobs.begin(); job != jobs.end();) {
            auto admitted = rt_admit((*job)->request.deadline);
            if (admitted == STATUS_OK && (*job)->canceled.pending()) {
                const auto peer = sys::source_info((*job)->request.reply.selector(), SOURCE_WRITE);
                if (peer.status != STATUS_OK || (peer.value & SOURCE_TERMINAL)) admitted = STATUS_CANCELED;
                else admitted = (*job)->canceled.arm(peer.value2).status;
            }
            if (admitted == STATUS_OK) {
                for (auto& item : (*job)->targets) again |= advance(item, (*job)->method, (*job)->request.deadline);
                if (std::ranges::any_of((*job)->targets, [](const auto& item) { return item.status == STATUS_PENDING; })) {
                    const auto due = (*job)->request.deadline;
                    if (due && (!deadline || due < deadline)) deadline = due;
                    ++job; continue;
                }
            }
            ubus_msg reply; reply.status = admitted == STATUS_OK ? complete(**job, reply) : admitted;
            const auto sent = (*job)->request.respond(std::move(reply));
            if (sent != STATUS_OK && sent != STATUS_CANCELED && sent != STATUS_CLOSED && sent != STATUS_STALE) return sent;
            for (auto& item : (*job)->targets) if (item.binding->status == STATUS_PENDING
                && item.binding.use_count() == 2 && item.mount->binding == item.binding) item.mount->binding.reset();
            job = jobs.erase(job); again = true;
        }
        if (!again) {
            status = ev_wait(rt_events(), deadline).status;
            if (status != STATUS_OK && status != STATUS_TIMED_OUT) return status;
        }
    }
}
