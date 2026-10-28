#include <io.hpp>
#include <vector>
#include <ubus/ubus.hpp>
#include <libk/fmt.hpp>
#include <source_location>

static cap_t output_cap;
struct ct_output { void write(const char* data, size_t size) {
    while (size) { const auto n = std::min(size, size_t{80}); ubus_msg message;
        message.data.put("data", std::as_bytes(std::span{data, n}));
        if (ubus_send(output_cap, message) != STATUS_OK) _thread_exit(STATUS_INTERNAL);
        data += n; size -= n;
    }
} };
static void check(bool value, std::source_location at = std::source_location::current()) {
    if (value) return;
    ct_output output;
    (void)libk::fmt::format_to<"[control] failed line {}\n">(output, at.line());
    _thread_exit(STATUS_INTERNAL);
}
static auto list(ubus_conn& owner) -> ubus_msg {
    ubus_msg response; check(owner.invoke("List", {}, response) == STATUS_OK); return response;
}
static auto find(ubus_conn& owner, std::string_view name) -> ubus_data {
    const auto response = list(owner);
    for (const auto item : response.data["items"].values()) if (item["name"].as<std::string_view>() == name) {
        ubus_data result; check(result.assign(item.bytes())); return result;
    }
    check(false); return {};
}
static auto action(ubus_conn& owner, ubus_value row, std::string_view name, bool stale = false) -> status_t {
    ubus_msg request, response;
    request.data.put("child_id", row["child_id"]).put("run_id", *row["run_id"].as<uint64_t>() + uint64_t(stale)).put("action", name);
    return owner.invoke("Action", std::move(request), response);
}
static void stopped(ubus_conn& owner, std::string_view name, std::string_view state = "idle") {
    const auto deadline = *_after(std::chrono::milliseconds{2000});
    while (find(owner, name)["state"].as<std::string_view>() != state)
        check(_deadline_status(deadline) == STATUS_OK);
}
static auto running(ubus_conn& owner, std::string_view name, uint64_t previous = 0, bool delivered = false) -> ubus_data {
    const auto deadline = *_after(std::chrono::milliseconds{2000});
    for (;;) {
        auto row = find(owner, name);
        if (row["state"].as<std::string_view>() == "running" && row["run_id"].as<uint64_t>().value_or(0) > previous
            && (!delivered || row["delivered"].as<bool>().value_or(false))) return row;
        if (_deadline_status(deadline) != STATUS_OK) {
            ct_output output;
            (void)libk::fmt::format_to<"[control] {} state={} run_id={} previous={} result={}\n">(output, name,
                row["state"].as<std::string_view>().value_or("?"), row["run_id"].as<uint64_t>().value_or(0), previous, row["result"].as<int64_t>().value_or(0));
            check(false);
        }
    }
}
static int backend() {
    ubus_conn parent; check(parent.bind(_startup().bootstrap) == STATUS_OK);
    ubus_msg setup; check(parent.invoke("Receive", {}, setup) == STATUS_OK);
    const auto payload = setup.data["message"];
    const auto pause = [&] {
        check(_notification_signal(setup.capability(payload["ready"])).status == STATUS_OK);
        check(_wait_one(setup.capability(payload["release"]), SOURCE_READY).status == STATUS_OK);
    };
    uint64_t listed{}, synced{};
    ubus_object files{"io.files"};
    check(files.add("List", {{"path", ubus_type::String}}, {{"epoch", ubus_type::U64}, {"items", ubus_type::Array}},
        [&](auto&, auto& r) { ++listed; r.data.put("epoch", uint64_t{1}).put("items", ubus_data{ubus_type::Array}); return STATUS_OK; }) == STATUS_OK);
    check(files.add("Sync", {}, {}, [&](auto&, auto&) { ++synced; return STATUS_OK; }) == STATUS_OK);
    check(files.add("Stats", {}, {{"listed", ubus_type::U64}, {"synced", ubus_type::U64}},
        [&](auto&, auto& r) { r.data.put("listed", listed).put("synced", synced); return STATUS_OK; }) == STATUS_OK);
    check(files.open() == STATUS_OK);
    if (!payload["bound"].as<bool>().value_or(false)) pause();
    auto connector = files.connector(); check(bool(connector));
    ubus_msg q, r; const auto cap = q.attach(connector->selector(), RIGHT_SEND | RIGHT_DUPLICATE); check(bool(cap));
    q.data.put("name", "files").cap("connector", *cap).put("interface", "io.files").put("version", uint64_t{1});
    check(parent.invoke("Publish", std::move(q), r) == STATUS_OK);
    if (payload["bound"].as<bool>().value_or(false)) pause();
    for (;;) {
        const auto status = files.poll(); check(status == STATUS_OK || status == STATUS_WOULD_BLOCK);
        if (status == STATUS_WOULD_BLOCK) check(_ev_wait(&_events()).status == STATUS_OK);
    }
}
static void vfs(ubus_conn& owner) {
    std::array<_cap_owner, 2> ready, release;
    const auto get = [&](std::string_view name, ubus_conn& connection) {
        ubus_msg q, r; q.deadline = *_after(std::chrono::milliseconds{3000}); q.data.put("name", name);
        check(owner.invoke("Get", std::move(q), r) == STATUS_OK);
        check(connection.bind(r.capability(r.data["connector"]), "io.files", 1, 1) == STATUS_OK);
    };
    for (unsigned i = 0; i < 2; ++i) {
        const auto a = _notification_create(_startup().alloc(), 1), b = _notification_create(_startup().alloc(), 1);
        check(a.status == STATUS_OK && b.status == STATUS_OK); ready[i] = _cap_owner{a.value}; release[i] = _cap_owner{b.value};
        ubus_msg q, r; ubus_data args{ubus_type::Array}, exports{ubus_type::Array}, setup, resources;
        args.push("backend"); exports.push("files"); resources.put("bytes", uint64_t{1024 * 1024}).put("caps", uint64_t{128});
        const auto signal = q.attach(ready[i].selector(), RIGHT_SIGNAL | RIGHT_DUPLICATE);
        const auto gate = q.attach(release[i].selector(), RIGHT_RECEIVE | RIGHT_DUPLICATE); check(signal && gate);
        setup.cap("ready", *signal).cap("release", *gate).put("bound", bool(i));
        q.data.put("name", i ? "vfs-bind" : "vfs-get").put("program", "/boot/control-test.elf").put("argv", args)
            .put("exports", exports).put("setup", setup).put("resources", resources).put("start", true);
        check(owner.invoke("Configure", std::move(q), r) == STATUS_OK);
        check(_wait_one(ready[i].selector(), SOURCE_READY, 0, *_after(std::chrono::milliseconds{3000})).status == STATUS_OK);
    }
    ubus_msg q, r; ubus_data routes, local, mounts{ubus_type::Array}, setup, exports{ubus_type::Array}, resources;
    local.put("local", true); routes.put("*", local); exports.push("*");
    for (const auto& [path, service, ro] : std::array{std::tuple{"/boot", "boot/files", true},
        std::tuple{"/get", "vfs-get/files", false}, std::tuple{"/bind", "vfs-bind/files", false}}) {
        ubus_data mount; mount.put("path", path).put("service", service).put("read_only", ro); mounts.push(mount);
    }
    setup.put("mounts", mounts); resources.put("bytes", uint64_t{2 * 1024 * 1024}).put("caps", uint64_t{128});
    q.data.put("name", "vfs-probe").put("program", "/boot/vfs.elf").put("routes", routes).put("exports", exports)
        .put("setup", setup).put("resources", resources).put("start", true);
    check(owner.invoke("Configure", std::move(q), r) == STATUS_OK);
    ubus_conn files, admin; get("vfs-probe/files", files); get("vfs-probe/admin", admin);
    std::array<ubus_call, 2> slow;
    for (unsigned i = 0; i < 2; ++i) {
        ubus_msg request; request.tag = *files.token("List"); request.deadline = *_after(std::chrono::milliseconds{3000});
        request.data.put("path", i ? "/bind" : "/get"); check(slow[i].begin(files.endpoint(), std::move(request)) == STATUS_OK);
    }
    q = {}; q.data.put("path", "/boot"); q.deadline = *_after(std::chrono::milliseconds{3000});
    check(files.invoke("List", std::move(q), r) == STATUS_OK && r.data["items"].type() == ubus_type::Array);
    q = {}; q.data.put("path", "/get");
    check(admin.invoke("Unmount", std::move(q), r) == STATUS_OK);
    for (auto& gate : release) check(_notification_signal(gate.selector()).status == STATUS_OK);
    for (auto& call : slow) check(call.wait(r) == STATUS_OK && r.status == STATUS_STALE);
    q = {}; q.data.put("path", "/get").put("service", "vfs-get/files").put("read_only", false);
    check(admin.invoke("Mount", std::move(q), r) == STATUS_OK);
    check(files.invoke("Sync", {}, r) == STATUS_OK);
    for (const auto name : {"vfs-get", "vfs-bind"}) {
        ubus_conn connection; get(std::string{name} + "/files", connection);
        check(connection.invoke("Stats", {}, r) == STATUS_OK);
        check(r.data["listed"].as<uint64_t>() == 1 && r.data["synced"].as<uint64_t>() == 1);
    }
    for (const auto name : {"vfs-probe", "vfs-get", "vfs-bind"}) {
        const auto row = find(owner, name); q = {}; q.data.put("child_id", row["child_id"]).put("run_id", row["run_id"])
            .put("version", row["config"]).put("action", "remove");
        check(owner.invoke("Action", std::move(q), r) == STATUS_OK);
    }
    for (auto& cap : ready) check(_object_destroy(cap.selector()).status == STATUS_OK);
    for (auto& cap : release) check(_object_destroy(cap.selector()).status == STATUS_OK);
    constexpr std::string_view marker{"[vfs] delayed Get/Bind, independent mount progress, pinned targets and Sync fanout ok\n"};
    ct_output{}.write(marker.data(), marker.size());
}
static void devices(ubus_conn& owner) {
    ubus_msg query, reply; query.data.put("name", "devices");
    check(owner.invoke("Lookup", std::move(query), reply) == STATUS_OK);
    ubus_conn device_owner; check(device_owner.bind(reply.capability(reply.data["connector"])) == STATUS_OK);
    query = {}; query.data.put("facet", "children");
    check(device_owner.invoke("Inspect", std::move(query), reply) == STATUS_OK);
    ubus_conn drivers; check(drivers.bind(reply.capability(reply.data["connector"])) == STATUS_OK);
    const auto memory = _memory_create(_startup().alloc(), io_limit + 4096, VM_READ | VM_WRITE);
    check(memory.status == STATUS_OK); _cap_owner buffer{memory.value};
    std::vector<std::byte> bytes(io_limit);
    const auto transfer = [&](std::string_view name, std::byte pattern, bool write) {
        ubus_msg request, response; request.deadline = *_after(std::chrono::milliseconds{3000}); request.data.put("name", std::string{name} + "/block");
        check(drivers.invoke("Get", std::move(request), response) == STATUS_OK);
        ubus_conn block; check(block.bind(response.capability(response.data["connector"]), "io.block", 1, 1) == STATUS_OK);
        check(block.invoke("Open", {}, response) == STATUS_OK);
        ubus_conn session; check(session.adopt(response.take(response.data["handle"])) == STATUS_OK);
        check(session.invoke("Stat", {}, response) == STATUS_OK);
        check(response.data["limit"].as<uint64_t>() == io_limit);
        auto id = *response.data["sequence"].as<uint64_t>();
        const auto command = [&](std::string_view method) {
            ubus_msg q, r; q.data.put("id", ++id);
            if (method != "Flush") {
                const auto cap = q.attach(buffer.selector(), RIGHT_MAP | RIGHT_DUPLICATE); check(bool(cap));
                q.data.put("offset", uint64_t{0}).put("buffer", ubus_buffer{*cap, 4093, io_limit});
            }
            check(session.invoke(method, std::move(q), r) == STATUS_OK);
            check(r.data["bytes"].as<uint64_t>() == (method == "Flush" ? 0 : io_limit));
        };
        if (write) {
            std::ranges::fill(bytes, pattern); check(_mem_write(buffer.selector(), 4093, bytes).status == STATUS_OK);
            command("Write"); command("Flush");
        }
        std::ranges::fill(bytes, std::byte{}); check(_mem_write(buffer.selector(), 4093, bytes).status == STATUS_OK);
        command("Read"); check(_mem_read(buffer.selector(), 4093, bytes).status == STATUS_OK);
        check(std::ranges::all_of(bytes, [&](auto byte) { return byte == pattern; }));
        check(session.invoke("Close", {}, response) == STATUS_OK);
    };
    transfer("block-8", std::byte{0xa5}, true); transfer("block-10", std::byte{0x5a}, true);
    const auto second = find(drivers, "block-10");
    check(action(drivers, second.view(), "stop") == STATUS_OK); stopped(drivers, "block-10");
    transfer("block-8", std::byte{0xa5}, false);
    check(action(drivers, second.view(), "start") == STATUS_OK); (void)running(drivers, "block-10", *second["run_id"].as<uint64_t>());
    transfer("block-10", std::byte{0x5a}, false);
    check(_object_destroy(buffer.selector()).status == STATUS_OK);
    constexpr std::string_view marker{"[devices] independent DMA, cross-page 64KiB I/O and driver retirement ok\n"};
    ct_output{}.write(marker.data(), marker.size());
}
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view{argv[1]} == "backend") return backend();
    const auto& env = _startup();
    ubus_conn bus, owner;
    check(bus.bind(env.bootstrap) == STATUS_OK);
    ubus_msg initial; check(bus.invoke("Receive", {}, initial) == STATUS_OK);
    output_cap = initial.capability(initial.data["message"]["stdout"]);
    const auto control = initial.capability(initial.data["message"]["control"]);
    check(owner.bind(control) == STATUS_OK);
    if (argc == 2 && std::string_view{argv[1]} == "devices") { devices(owner); return 0; }
    if (argc == 2 && std::string_view{argv[1]} == "vfs") { vfs(owner); return 0; }
    ubus_conn readonly, creator;
    check(readonly.bind(initial.capability(initial.data["message"]["observe"])) == STATUS_OK);
    check(creator.bind(initial.capability(initial.data["message"]["create"])) == STATUS_OK);
    // Descriptions and raw tokens must enforce the same permissions.
    for (const auto name : {"Resource", "Get", "Configure", "Spawn", "Action", "Send", "CloseCap", "Revoke"}) {
        check(!readonly.token(name));
        ubus_msg raw, denied; raw.tag = *owner.token(name);
        ubus_call call; check(call.begin(readonly.endpoint(), std::move(raw)) == STATUS_OK);
        check(call.wait(denied) == STATUS_OK && denied.status == STATUS_DENIED);
    }
    ubus_msg permission, result;
    permission.data.put("rights", uint64_t{3});
    check(readonly.invoke("Delegate", std::move(permission), result) == STATUS_DENIED);
    permission = {}; permission.data.put("rights", uint64_t{1});
    check(readonly.invoke("Delegate", std::move(permission), result) == STATUS_OK);
    ubus_conn lease; check(lease.bind(result.capability(result.data["connector"])) == STATUS_OK);
    check(lease.invoke("Delegate", {}, result) == STATUS_OK);
    ubus_conn descendant; check(descendant.bind(result.capability(result.data["connector"])) == STATUS_OK);
    check(lease.invoke("Close", {}, result) == STATUS_OK);
    check(lease.invoke("List", {}, result) == STATUS_CLOSED);
    check(descendant.invoke("List", {}, result) == STATUS_CLOSED);
    // Closing the last handle reclaims independent grants, including Bind's
    // retained connection. A live bound connection survives the original slot.
    for (size_t i = 0; i < 160; ++i) {
        check(readonly.invoke("Delegate", {}, result) == STATUS_OK);
        ubus_conn temporary;
        check(temporary.bind(result.capability(result.data["connector"])) == STATUS_OK);
        result = {};
        check(temporary.invoke("List", {}, result) == STATUS_OK);
        check(temporary.close() == STATUS_OK);
    }
    // Numeric routing directives cannot manufacture namespace authority.
    permission = {}; ubus_data routes, escape;
    escape.put("manager", uint64_t{3}); routes.put("escape", escape);
    permission.data.put("name", "forbidden").put("program", "/boot/sleep.elf").put("routes", routes);
    check(creator.invoke("Configure", std::move(permission), result) == STATUS_DENIED);
    permission = {}; ubus_data oversized{ubus_type::Array}; oversized.push(std::string(66 * 1024, 'x'));
    permission.data.put("name", "oversized").put("program", "/boot/sleep.elf").put("argv", oversized);
    check(creator.invoke("Configure", std::move(permission), result) == STATUS_EXHAUSTED);
    const auto shell = find(owner, "shell");
    ubus_msg hidden, denied;
    hidden.data.put("child_id", shell["child_id"]).put("run_id", shell["run_id"]).put("facet", "caps");
    check(readonly.invoke("Inspect", std::move(hidden), denied) == STATUS_DENIED);
    {
        hidden = {}; hidden.data.put("rights", uint64_t{3});
        check(owner.invoke("Delegate", std::move(hidden), denied) == STATUS_OK);
        ubus_conn ordinary; check(ordinary.bind(denied.capability(denied.data["connector"])) == STATUS_OK);
        check(!ordinary.token("CloseCap") && !ordinary.token("Revoke"));
        hidden = {}; hidden.data.put("child_id", shell["child_id"]).put("run_id", shell["run_id"]).put("facet", "caps");
        check(ordinary.invoke("Inspect", std::move(hidden), denied) == STATUS_DENIED);
        check(ordinary.invoke("Close", {}, denied) == STATUS_OK);
    }
    hidden = {}; hidden.data.put("rights", uint64_t{9});
    check(readonly.invoke("Delegate", std::move(hidden), denied) == STATUS_DENIED);
    check(shell["cpu_status"].as<int64_t>() == STATUS_OK);
    check(shell["cpu_time"].as<uint64_t>().has_value());
    check(shell["sampled"].as<uint64_t>().has_value());

    check(shell["runtime"].as<uint64_t>().has_value());
    check(shell["committed"].as<uint64_t>().value_or(0) > 0);
    check(shell["resident"].as<uint64_t>().value_or(0) > 0);
    check(action(owner, shell.view(), "stop", true) == STATUS_STALE);
    ubus_msg query, response;
    query.data.put("name", "no-activation").put("program", "/boot/sleep.elf");
    check(owner.invoke("Configure", std::move(query), response) == STATUS_OK);
    query = {}; query.data.put("name", "no-activation/probe").put("activate", false);
    check(owner.invoke("Get", std::move(query), response) == STATUS_NOT_FOUND);
    auto idle = find(owner, "no-activation"); check(idle["run_id"].as<uint64_t>() == 0);
    query = {}; query.data.put("child_id", idle["child_id"]).put("run_id", idle["run_id"]).put("version", idle["config"]).put("action", "remove");
    check(owner.invoke("Action", std::move(query), response) == STATUS_OK);
    query = {}; query.data.put("child_id", shell["child_id"]).put("run_id", shell["run_id"]).put("facet", "caps");
    check(owner.invoke("Inspect", std::move(query), response) == STATUS_OK);
    bool found{};
    for (const auto cap : response.data["items"].values()) {
        check(cap["selector"].as<uint64_t>().value_or(0) != 0);
        check(cap["kind"].as<uint64_t>().has_value());
        check(cap["words"].vector<uint64_t>()->size() == 6);
        found = true;
    }
    check(found);
    query = {};
    query.data.put("child_id", shell["child_id"]).put("run_id", shell["run_id"]).put("facet", "caps")
        .put("cursor", uint64_t{1}).put("epoch", UINT64_MAX);
    check(owner.invoke("Inspect", std::move(query), response) == STATUS_STALE);

    // This service belongs to the selected namespace, independently of this caller.
    ubus_data args{ubus_type::Array}; args.push("10000");
    query = {};
    query.data.put("name", "control-sleep").put("program", "/boot/sleep.elf").put("argv", args);
    check(owner.invoke("Spawn", std::move(query), response) == STATUS_OK);
    ubus_conn scoped;
    check(scoped.bind(response.capability(response.data["connector"])) == STATUS_OK);
    const auto child = running(owner, "control-sleep");
    query = {}; query.data.put("child_id", shell["child_id"]).put("facet", "overview");
    check(scoped.invoke("Inspect", std::move(query), response) == STATUS_DENIED);
    query = {}; query.data.put("execution", true).put("rights", uint64_t{1});
    check(scoped.invoke("Delegate", std::move(query), response) == STATUS_OK);
    ubus_conn execution; check(execution.bind(response.capability(response.data["connector"])) == STATUS_OK);
    ubus_conn watcher; check(watcher.bind(control) == STATUS_OK);
    query = {}; query.data.put("child_id", child["child_id"]).put("run_id", child["run_id"]).put("epoch", uint64_t{0});
    check(watcher.invoke("Changed", std::move(query), response) == STATUS_OK);
    const auto epoch = *response.data["epoch"].as<uint64_t>();
    query = {}; query.data.put("child_id", child["child_id"]).put("run_id", child["run_id"]).put("epoch", epoch);
    query.deadline = *_after(std::chrono::milliseconds{2000});
    check(watcher.begin("Changed", std::move(query)) == STATUS_OK);
    check(action(owner, child.view(), "stop") == STATUS_OK);
    for (;;) {
        const auto status = watcher.poll(response);
        if (status == STATUS_OK) {
            if (response.status == STATUS_CLOSED) break;
            check(response.status == STATUS_OK && response.data["epoch"].as<uint64_t>() != epoch);
            query = {}; query.data.put("child_id", child["child_id"]).put("run_id", child["run_id"])
                .put("epoch", response.data["epoch"]); query.deadline = *_after(std::chrono::milliseconds{2000});
            const auto next = watcher.begin("Changed", std::move(query)); check(next == STATUS_OK);
            continue;
        }
        check(status == STATUS_WOULD_BLOCK); check(_ev_wait(&_events(), *_after(std::chrono::milliseconds{2000})).status == STATUS_OK);
    }
    stopped(owner, "control-sleep");
    check(action(owner, child.view(), "start") == STATUS_OK);
    const auto replaced = running(owner, "control-sleep", *child["run_id"].as<uint64_t>());
    check(*replaced["run_id"].as<uint64_t>() > *child["run_id"].as<uint64_t>());
    query = {}; query.data.put("facet", "overview");
    check(execution.invoke("Inspect", std::move(query), response) == STATUS_CLOSED);
    check(scoped.invoke("Inspect", {}, response) == STATUS_BAD_ARGS); // Still live across generations.
    check(action(owner, child.view(), "stop") == STATUS_STALE);
    check(action(owner, replaced.view(), "stop") == STATUS_OK);
    stopped(owner, "control-sleep");
    query = {}; query.data.put("action", "remove").put("run_id", replaced["run_id"]).put("version", replaced["config"]);
    check(scoped.invoke("Action", std::move(query), response) == STATUS_OK);
    for (;;) {
        query = {}; query.data.put("facet", "overview");
        const auto status = scoped.invoke("Inspect", std::move(query), response);
        if (status == STATUS_CLOSED) break;
        check(status == STATUS_OK);
    }
    query = {}; query.data.put("name", "control-sleep");
    check(owner.invoke("Lookup", std::move(query), response) == STATUS_NOT_FOUND);
    // Create-only callers receive control of their result, never their siblings.
    query = {}; query.data.put("name", "created-peer").put("program", "/boot/sleep.elf").put("argv", args);
    check(creator.invoke("Spawn", std::move(query), response) == STATUS_OK);
    ubus_conn peer; check(peer.bind(response.capability(response.data["connector"])) == STATUS_OK);
    query = {}; query.data.put("child_id", shell["child_id"]).put("facet", "overview");
    check(peer.invoke("Inspect", std::move(query), response) == STATUS_DENIED);
    const auto created = running(owner, "created-peer");
    query = {}; query.data.put("run_id", created["run_id"]).put("action", "remove").put("version", created["config"]);
    check(peer.invoke("Action", std::move(query), response) == STATUS_OK);
    _ch_pair channel; check(channel.open(env.alloc(), 1, 0) == STATUS_OK);
    const auto minted = _channel_mint(channel.ends[1].selector(), env.cspace, 1, RIGHT_SEND);
    check(minted.status == STATUS_OK);
    _cap_owner sender{minted.value};
    check(channel.ends[1].close() == STATUS_OK);
    query = {}; ubus_data setup;
    const auto unused_idx = query.give(std::move(sender));
    if (!unused_idx) return unused_idx.error();
    setup.cap("unused", *unused_idx);
    _ch_pair revocable; check(revocable.open(env.alloc(), 1, 0) == STATUS_OK);
    const auto branch_grant = _channel_mint(revocable.ends[1].selector(), env.cspace, 1, RIGHT_SEND | RIGHT_DUPLICATE | RIGHT_REVOKE);
    check(branch_grant.status == STATUS_OK); _cap_owner branch_cap{branch_grant.value};
    check(revocable.ends[1].close() == STATUS_OK);
    const auto grant_idx = query.attach(branch_grant.value, RIGHT_SEND | RIGHT_DUPLICATE | RIGHT_REVOKE);
    if (!grant_idx) return grant_idx.error();
    setup.cap("revocable", *grant_idx);
    ubus_data hold{ubus_type::Array}; hold.push("hold");
    query.data.put("name", "linear-sleep").put("program", "/boot/supervisor.elf").put("argv", hold).put("setup", setup);
    check(owner.invoke("Spawn", std::move(query), response) == STATUS_OK);
    const auto linear = running(owner, "linear-sleep", 0, true);
    query = {}; query.data.put("child_id", linear["child_id"]).put("run_id", linear["run_id"]).put("facet", "caps");
    check(owner.invoke("Inspect", std::move(query), response) == STATUS_OK);
    cap_t selector{};
    for (const auto cap : response.data["items"].values())
        if (cap["kind"].as<uint64_t>() == ObjChannel
            && cap["rights"].as<uint64_t>() == (RIGHT_SEND | RIGHT_DUPLICATE | RIGHT_REVOKE))
            selector = *cap["selector"].as<cap_t>();
    check(selector != 0);
    query = {}; query.data.put("child_id", linear["child_id"]).put("run_id", linear["run_id"]).put("selector", selector);
    check(owner.invoke("Revoke", std::move(query), response) == STATUS_OK);
    check(response.data["state"].as<uint64_t>().value_or(0) & (STATE_CLOSING | STATE_DRAINED));
    check(_wait_one(revocable.ends[0].selector(), SOURCE_TERMINAL, 0, *_after(std::chrono::milliseconds{1000}), 0, SOURCE_READ).status == STATUS_OK);
    const auto source = _source_info(channel.ends[0].selector(), SOURCE_READ);
    check(source.status == STATUS_OK && !(source.value & SOURCE_TERMINAL));
    check(action(owner, linear.view(), "stop") == STATUS_OK);
    stopped(owner, "linear-sleep");
    check(_wait_one(channel.ends[0].selector(), SOURCE_TERMINAL, 0, *_after(std::chrono::milliseconds{1000}), 0, SOURCE_READ).status == STATUS_OK);

    query = {}; query.data.put("name", "exec");
    check(bus.invoke("Get", std::move(query), response) == STATUS_OK);
    auto executable = response.take(response.data["connector"]);
    // Runtime-created manager: configuration and an explicit executable view
    // replace any requirement for a boot archive or a special manager identity.
    query = {}; ubus_data branch_setup, config, recipes{ubus_type::Array}, foreign, branch_routes, route_exec, limits;
    foreign.put("name", "foreign").put("program", "/boot/foreign.elf").put("restart", true);
    ubus_data exports{ubus_type::Array}; exports.push("math"); foreign.put("exports", exports); recipes.push(foreign);
    config.put("children", recipes); branch_setup.put("config", config);
    const auto exec_idx = query.attach(executable.selector(), RIGHT_SEND | RIGHT_DUPLICATE);
    if (!exec_idx) return exec_idx.error();
    route_exec.cap("connector", *exec_idx); branch_routes.put("exec", route_exec);
    limits.put("bytes", uint64_t{16 * 1024 * 1024}).put("caps", uint64_t{512}).put("children", true);
    query.data.put("name", "branch").put("program", "/boot/mgr.elf").put("setup", branch_setup).put("routes", branch_routes).put("resources", limits);
    check(owner.invoke("Spawn", std::move(query), response) == STATUS_OK);
    const auto root = running(owner, "branch");
    ubus_conn branch;
    const auto attach = [&] {
        const auto deadline = *_after(std::chrono::milliseconds{2000});
        for (;;) {
            query = {}; query.data.put("child_id", root["child_id"]).put("facet", "children");
            const auto status = owner.invoke("Inspect", std::move(query), response);
            if (status == STATUS_OK) { check(branch.close() == STATUS_OK); check(branch.bind(response.capability(response.data["connector"])) == STATUS_OK); return; }
            check(status == STATUS_NOT_FOUND && _deadline_status(deadline) == STATUS_OK);
        }
    };
    attach();
    auto child_foreign = find(branch, "foreign"); check(child_foreign["run_id"].as<uint64_t>() == 0);
    const auto connect = [&] {
        query = {}; query.data.put("name", "foreign/math");
        check(branch.invoke("Get", std::move(query), response) == STATUS_OK);
        ubus_conn calculator; check(calculator.bind(response.capability(response.data["connector"])) == STATUS_OK);
        query = {}; query.data.put("a", uint64_t{11}).put("b", uint64_t{31});
        check(calculator.invoke("Add", std::move(query), response) == STATUS_OK && response.data["sum"].as<uint64_t>() == 42);
    };
    connect();
    child_foreign = running(branch, "foreign");
    check(action(branch, child_foreign.view(), "start") == STATUS_OK); // Commit its explicit restart policy.
    query = {}; query.data.put("name", "nested").put("program", "/boot/sleep.elf").put("argv", args);
    check(branch.invoke("Spawn", std::move(query), response) == STATUS_OK);
    check(action(owner, root.view(), "restart") == STATUS_OK);
    auto fresh_root = running(owner, "branch", *root["run_id"].as<uint64_t>()); attach();
    (void)running(branch, "foreign"); connect();
    check(find(branch, "nested")["run_id"].as<uint64_t>() == 0); // One-shot execution is not replayed.
    child_foreign = find(branch, "foreign");
    query = {}; query.data.put("child_id", child_foreign["child_id"]).put("run_id", child_foreign["run_id"]).put("version", child_foreign["config"]).put("action", "remove");
    check(branch.invoke("Action", std::move(query), response) == STATUS_OK);
    check(action(owner, fresh_root.view(), "restart") == STATUS_OK);
    fresh_root = running(owner, "branch", *fresh_root["run_id"].as<uint64_t>()); attach();
    query = {}; query.data.put("name", "foreign"); check(branch.invoke("Lookup", std::move(query), response) == STATUS_NOT_FOUND);
    query = {}; query.data.put("child_id", fresh_root["child_id"]).put("run_id", fresh_root["run_id"]).put("version", fresh_root["config"]).put("action", "remove");
    check(owner.invoke("Action", std::move(query), response) == STATUS_OK); check(branch.close() == STATUS_OK);
    for (const auto deadline = *_after(std::chrono::milliseconds{2000});;) {
        query = {}; query.data.put("name", "branch");
        const auto status = owner.invoke("Lookup", std::move(query), response);
        if (status == STATUS_NOT_FOUND) break;
        check(status == STATUS_OK && _deadline_status(deadline) == STATUS_OK);
    }
    // The failed supervisor leaves a running descendant holding the last Send
    // capability. Reusing the same budget proves retirement, not just exit.
    for (const auto name : {"subtree-a", "subtree-b"}) {
        _ch_pair proof; check(proof.open(env.alloc(), 1, 0) == STATUS_OK);
        const auto cap = _channel_mint(proof.ends[1].selector(), env.cspace, 1, RIGHT_SEND);
        check(cap.status == STATUS_OK); check(proof.ends[1].close() == STATUS_OK);
        query = {}; setup = {};
        const auto proof_idx = query.give(_cap_owner{cap.value});
        if (!proof_idx) return proof_idx.error();
        setup.cap("proof", *proof_idx);
        const auto created = _notification_create(env.alloc(), 1); check(created.status == STATUS_OK);
        _cap_owner crash{created.value};
        const auto crash_idx = query.attach(crash.selector(), RIGHT_RECEIVE | RIGHT_DUPLICATE);
        if (!crash_idx) return crash_idx.error();
        setup.cap("crash", *crash_idx);
        ubus_data resources; resources.put("children", true);
        query.data.put("name", name).put("program", "/boot/supervisor.elf").put("setup", setup).put("resources", resources);
        ubus_data routes, upstream;
        const auto exec_idx = query.attach(executable.selector(), RIGHT_SEND | RIGHT_DUPLICATE);
        if (!exec_idx) return exec_idx.error();
        upstream.cap("connector", *exec_idx); routes.put("exec", upstream); query.data.put("routes", routes);
        check(owner.invoke("Spawn", std::move(query), response) == STATUS_OK);
        const auto ready = _wait_one(proof.ends[0].selector(), SOURCE_READY, 0, *_after(std::chrono::milliseconds{2000}), 0, SOURCE_READ).status;
        if (ready != STATUS_OK) { const auto facts = find(owner, name); ct_output output;
            (void)libk::fmt::format_to<"[control] {} ready={} state={} result={}\n">(output, std::string_view{name}, ready,
                facts["state"].as<std::string_view>().value_or("?"), facts["result"].as<int64_t>().value_or(0)); }
        check(ready == STATUS_OK);
        _ch_packet message; const auto received = _ch_try_recv(proof.ends[0].selector(), message).status;
        if (received != STATUS_OK) {
            stopped(owner, name, "failed");
            const auto row = find(owner, name);
            ct_output output;
            (void)libk::fmt::format_to<"[control] subtree receive={} exit={}\n">(output, received, row["result"].as<int64_t>().value_or(0));
        }
        check(received == STATUS_OK);
        check(message.word_count == 1 && message.words[0] == 42);
        check(!(_source_info(proof.ends[0].selector(), SOURCE_READ).value & SOURCE_TERMINAL));
        check(_notification_signal(crash.selector()).status == STATUS_OK);
        check(_wait_one(proof.ends[0].selector(), SOURCE_TERMINAL, 0, *_after(std::chrono::milliseconds{2000}), 0, SOURCE_READ).status == STATUS_OK);
        stopped(owner, name, "failed");
        check(find(owner, name)["result"].as<int64_t>() == STATUS_INTERNAL);
    }
    // Bound copies share admission. Exhausting Changed must not block List,
    // and cancellation must release the held replies and native watches.
    for (auto* connection : {&owner, &creator, &lease, &descendant, &scoped, &execution, &watcher, &peer, &branch}) check(connection->close() == STATUS_OK);
    const auto before = list(readonly); const auto version = *before.data["epoch"].as<uint64_t>();
    std::vector<std::unique_ptr<ubus_conn>> waiting;
    for (size_t i = 0; i < 33; ++i) {
        auto next = std::make_unique<ubus_conn>();
        const auto bound = next->bind(initial.capability(initial.data["message"]["observe"]));
        if (bound != STATUS_OK) { ct_output output; (void)libk::fmt::format_to<"[control] quota bind {}: {}\n">(output, i, bound); }
        check(bound == STATUS_OK);
        query = {}; query.data.put("epoch", version); query.deadline = *_after(std::chrono::milliseconds{2000});
        check(next->begin("Changed", std::move(query)) == STATUS_OK); waiting.push_back(std::move(next));
    }
    (void)list(readonly);
    for (;;) {
        const auto status = waiting.back()->poll(response);
        if (status == STATUS_OK) {
            if (response.status != STATUS_EXHAUSTED) { ct_output output;
                (void)libk::fmt::format_to<"[control] quota status={} epoch={} expected={}\n">(output, response.status, response.data["epoch"].as<uint64_t>().value_or(0), version); }
            check(response.status == STATUS_EXHAUSTED); break;
        }
        check(status == STATUS_WOULD_BLOCK); check(_ev_wait(&_events(), *_after(std::chrono::milliseconds{2000})).status == STATUS_OK);
    }
    waiting.clear(); (void)list(readonly);
    check(creator.bind(initial.capability(initial.data["message"]["create"])) == STATUS_OK);
    query = {}; query.data.put("name", "survivor").put("program", "/boot/sleep.elf").put("argv", args);
    check(creator.invoke("Spawn", std::move(query), response) == STATUS_OK);
    ct_output output;
    constexpr std::string_view marker{"[control] scoped badges, stale instances/cursors and real VM/quota stats ok\n"};
    output.write(marker.data(), marker.size());
    return STATUS_OK;
}
