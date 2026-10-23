#include <ubus/ubus.hpp>
#include <map>
#include <set>

enum : uint64_t { vf_lookup = 1, vf_modify = 2, vf_execute = 4, vf_admin = 8 };
enum : uint64_t { vf_read = 1, vf_write = 2, vf_create = 4, vf_truncate = 8, vf_exclusive = 16, vf_append = 32 };
enum : status_t { vf_not_dir = -255, vf_cross_device = -249 };
static auto vf_path(std::string_view path, std::string_view base = "/") -> std::string {
    std::string result = path.starts_with('/') ? std::string{path} : std::string{base} + "/" + std::string{path};
    size_t size{}, cursor{};
    while (cursor < result.size()) {
        const auto begin = cursor; while (cursor < result.size() && result[cursor] != '/') ++cursor;
        const std::string_view part{result.data() + begin, cursor - begin};
        if (part == "..") { while (size && result[size - 1] != '/') --size; if (size) --size; }
        else if (!part.empty() && part != ".") { if (size) result[size++] = '/'; ::memmove(result.data() + size, part.data(), part.size()); size += part.size(); }
        ++cursor;
    }
    result.resize(size); result.insert(0, "/"); return result;
}

struct vf_mount {
    std::string service;
    bool read_only{};
    uint64_t generation{};
    std::unique_ptr<ubus_conn> connection;
};
static std::map<std::string, vf_mount, std::less<>> mounts;
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
static auto connect(vf_mount& mount, uint64_t generation = 0) -> status_t {
    if (generation && (!mount.connection || mount.generation != generation)) return STATUS_STALE;
    if (mount.connection) {
        const auto fact = sys::source_info(mount.connection->endpoint(), SOURCE_WRITE);
        if (fact.status == STATUS_OK && !(fact.value & SOURCE_TERMINAL)) return STATUS_OK;
        mount.connection.reset(); ++epoch;
        if (generation) return STATUS_STALE;
    }
    auto connection = std::make_unique<ubus_conn>();
    ubus_msg query, reply; query.data.put("name", mount.service);
    auto status = bus.invoke("Get", std::move(query), reply);
    if (status == STATUS_OK) status = connection->bind(reply.capability(reply.data["connector"]), "io.files", 1, 1);
    if (status != STATUS_OK) return status;
    mount.connection = std::move(connection); mount.generation = ++next_binding;
    return STATUS_OK;
}
static auto invoke(vf_mount& mount, std::string_view method, ubus_msg request, ubus_msg& response) -> status_t {
    const auto status = mount.connection->invoke(method, std::move(request), response);
    if (status == STATUS_CLOSED || status == STATUS_PEER_CLOSED || status == STATUS_INVALID_CAP || status == STATUS_STALE) {
        mount.connection.reset(); ++epoch;
    }
    // A failure changes future resolution; never replay an accepted mutation.
    return status;
}
static auto control(std::string_view method, ubus_request& request, ubus_msg& response) -> status_t {
    const auto path = vf_path(request.data["path"].as<std::string_view>().value_or("/"));
    auto mount = route(path);
    if (mount == mounts.end()) return method == "Sync" ? STATUS_OK : STATUS_NOT_FOUND;
    const bool mutate = method == "Open" ? (*request.data["flags"].as<uint64_t>() & vf_write) != 0
        : method == "Remove" || method == "Mkdir" || method == "Rename" || method == "Sync";
    if (mutate && (!(request.facet() & vf_modify) || mount->second.read_only)) return STATUS_DENIED;
    if (method == "Map") {
        const auto perms = *request.data["perms"].as<word_t>();
        if (perms != VM_READ && perms != (VM_READ | VM_EXECUTE)) return STATUS_BAD_ARGS;
        if ((perms & VM_EXECUTE) && !(request.facet() & vf_execute)) return STATUS_DENIED;
    }
    if (method == "Open") {
        const auto flags = *request.data["flags"].as<uint64_t>();
        if (!(flags & (vf_read | vf_write)) || flags & ~(vf_read | vf_write | vf_create | vf_truncate | vf_exclusive | vf_append)
            || ((flags & ~(vf_read | vf_write)) && !(flags & vf_write))) return STATUS_BAD_ARGS;
    }
    ubus_msg forwarded, result; forwarded.deadline = request.deadline;
    if (method == "Rename") {
        const auto target = vf_path(*request.data["to"].as<std::string_view>());
        if (route(target) != mount) return vf_cross_device;
        forwarded.data.put("from", relative(path, mount->first)).put("to", relative(target, mount->first));
    } else if (method != "Sync" && method != "DeviceId" && method != "VolumeId") forwarded.data.put("path", relative(path, mount->first));
    if (method == "Map") forwarded.data.put("perms", *request.data["perms"].as<word_t>());
    if (method == "Open") {
        forwarded.data.put("flags", *request.data["flags"].as<uint64_t>());
    }
    if (method == "Document") forwarded.data.put("writable", bool((request.facet() & vf_modify) && !mount->second.read_only));
    if (const auto budget = request.data["budget"]) {
        const auto index = forwarded.attach(request.capability(budget), RIGHT_CREATE | RIGHT_INSPECT | RIGHT_DUPLICATE);
        if (!index) return index.error();
        forwarded.data.cap("budget", *index);
    }
    auto status = connect(mount->second);
    if (status != STATUS_OK) return status;
    status = invoke(mount->second, method, std::move(forwarded), result);
    if (status != STATUS_OK) { response.data = std::move(result.data); return status; }
    response.data = std::move(result.data);
    if (method == "Map" || method == "Open" || method == "Document") {
        const auto value = response.data[method == "Map" ? "buffer" : "handle"];
        auto cap = result.take(value);
        if (!cap || result.count != 1) return STATUS_PEER_FAULT;
        const auto index = method == "Map" ? response.offer(std::move(cap), RIGHT_MAP | RIGHT_DUPLICATE | RIGHT_DELEGATE)
            : response.give(std::move(cap));
        if (!index) return index.error();
    }
    return STATUS_OK;
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
    const auto path = vf_path(*request.data["path"].as<std::string_view>());
    const auto cursor = request.data["cursor"];
    if (cursor && !cursor.data().empty() && cursor["epoch"].as<uint64_t>() != epoch) return STATUS_STALE;
    const auto covered = children(path);
    auto mount = route(path);
    ubus_data items{ubus_type::Array};
    const bool first = !cursor || cursor.data().empty();
    if (first && (!covered.empty() || mount == mounts.end())) {
        if (covered.empty() && path != "/") return vf_not_dir;
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
    auto status = connect(mount->second, generation);
    if (status != STATUS_OK) return status;
    ubus_msg forwarded, result; forwarded.deadline = request.deadline;
    forwarded.data.put("path", relative(path, mount->first));
    if (!first) {
        if (cursor["backend"].type() != ubus_type::Record) return STATUS_BAD_ARGS;
        forwarded.data.put("cursor", cursor["backend"]);
    }
    status = invoke(mount->second, "List", std::move(forwarded), result);
    if (status != STATUS_OK) return status;
    if (result.data["items"].type() != ubus_type::Array) return STATUS_PEER_FAULT;
    for (const auto item : result.data["items"].values()) {
        const auto name = item["name"].as<std::string_view>();
        if (!name) return STATUS_PEER_FAULT;
        if (!covered.contains(std::string{*name})) items.push(item);
    }
    response.data.put("epoch", epoch).put("items", items);
    if (const auto backend = result.data["cursor"]) {
        if (backend.type() != ubus_type::Record) return STATUS_PEER_FAULT;
        ubus_data next; next.put("epoch", epoch).put("generation", mount->second.generation).put("backend", backend);
        response.data.put("cursor", next);
    }
    return STATUS_OK;
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
        vf_mount mount{std::string{*service}, *read_only, 0, {}};
        mounts.emplace(vf_path(*path), std::move(mount));
    }
    for (const auto result : {
        directory.add("Map", {{"path", ubus_type::String}, {"perms", ubus_type::U64}}, {{"buffer", ubus_type::Buffer}}, [](auto& q, auto& r) { return control("Map", q, r); }, vf_lookup),
        directory.add("Document", {{"path", ubus_type::String}, {"budget", ubus_type::Cap, true}}, {{"handle", ubus_type::Cap}}, [](auto& q, auto& r) { return control("Document", q, r); }, vf_lookup),
        directory.add("Open", {{"path", ubus_type::String}, {"flags", ubus_type::U64}, {"budget", ubus_type::Cap, true}}, {{"handle", ubus_type::Cap}}, [](auto& q, auto& r) { return control("Open", q, r); }, vf_lookup),
        directory.add("List", {{"path", ubus_type::String}, {"cursor", ubus_type::Record, true}}, {{"epoch", ubus_type::U64}, {"items", ubus_type::Array}, {"cursor", ubus_type::Record, true}}, list, vf_lookup),
        directory.add("Remove", {{"path", ubus_type::String}}, {}, [](auto& q, auto& r) { return control("Remove", q, r); }, vf_modify),
        directory.add("Mkdir", {{"path", ubus_type::String}}, {}, [](auto& q, auto& r) { return control("Mkdir", q, r); }, vf_modify),
        directory.add("Rename", {{"from", ubus_type::String}, {"to", ubus_type::String}}, {}, [](auto& q, auto& r) {
            ubus_data input; input.put("path", q.data["from"]).put("to", q.data["to"]); q.data = std::move(input); return control("Rename", q, r);
        }, vf_modify),
        directory.add("Sync", {}, {}, [](auto& q, auto& r) { return control("Sync", q, r); }, vf_modify),
        directory.add("DeviceId", {}, {{"id", ubus_type::Bytes}}, [](auto& q, auto& r) { return control("DeviceId", q, r); }, vf_lookup),
        directory.add("VolumeId", {}, {{"id", ubus_type::Bytes}}, [](auto& q, auto& r) { return control("VolumeId", q, r); }, vf_lookup),
        directory.add("Mount", {{"path", ubus_type::String}, {"service", ubus_type::String}, {"read_only", ubus_type::Bool}}, {}, [](auto& q, auto&) {
            const auto path = vf_path(*q.data["path"].template as<std::string_view>());
            mounts.insert_or_assign(path, vf_mount{std::string{*q.data["service"].template as<std::string_view>()}, *q.data["read_only"].template as<bool>(), 0, {}});
            ++epoch; return STATUS_OK;
        }, vf_admin),
        directory.add("Unmount", {{"path", ubus_type::String}}, {}, [](auto& q, auto&) {
            if (!mounts.erase(vf_path(*q.data["path"].template as<std::string_view>()))) return STATUS_NOT_FOUND;
            ++epoch; return STATUS_OK;
        }, vf_admin)}) if (result != STATUS_OK) return result;
    if ((status = directory.open()) != STATUS_OK) return status;
    for (const auto& [name, rights] : std::array<std::pair<std::string_view, word_t>, 4>{{
        {"files", vf_lookup | vf_modify}, {"read", vf_lookup}, {"exec", vf_lookup | vf_execute}, {"admin", vf_admin}}}) {
        auto connector = directory.connector(rights); if (!connector) return connector.error();
        ubus_msg request, response;
        const auto peer_idx = request.attach(connector->selector(), RIGHT_SEND | RIGHT_DUPLICATE);
        if (!peer_idx) return peer_idx.error();
        request.data.put("name", name).put("interface", "io.files").put("version", uint64_t{1})
            .cap("connector", *peer_idx);
        status = bus.invoke("Publish", std::move(request), response); if (status != STATUS_OK) return status;
    }
    for (;;) {
        status = directory.poll();
        if (status != STATUS_OK && status != STATUS_WOULD_BLOCK) return status;
        if (status == STATUS_WOULD_BLOCK && (status = ev_wait(env.events).status) != STATUS_OK) return status;
    }
}
