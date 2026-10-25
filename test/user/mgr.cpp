#include <libk/cpio.hpp>
#include <mgr/mgr.hpp>
#include "../../apps/devices/fw.hpp"
#include "../../apps/uart/uart.hpp"
#include <libk/fmt.hpp>

static auto vm_permissions() -> status_t {
    const auto& env = _startup();
    auto readonly = _vm_area::create(env.alloc(), _vm(), 0, 4096, VM_READ);
    auto writable = _vm_area::create(env.alloc(), _vm(), 0, 4096);
    auto staging = _vm_area::create(env.alloc(), _vm(), 0, 4096, VM_READ | VM_WRITE | VM_EXECUTE, VM_READ | VM_WRITE);
    if (!readonly || !writable || !staging) return STATUS_INTERNAL;
    if (*reinterpret_cast<const volatile uint64_t*>(readonly->address) != 0) return STATUS_INTERNAL;
    *reinterpret_cast<volatile uint64_t*>(writable->address) = 42;
    if (*reinterpret_cast<const volatile uint64_t*>(writable->address) != 42) return STATUS_INTERNAL;
    const auto info = _object_info(env.vspace); if (info.status != STATUS_OK) return info.status;
    const auto object = reinterpret_cast<const ObjectInfo*>(_ipc())->object_id;
    uint64_t cursor{}, epoch{}; size_t found{};
    do {
        const auto status = _scope_query(env.scope, object, SCOPE_LINKS, cursor, epoch);
        if (status.status != STATUS_OK) return status.status;
        const auto page = *reinterpret_cast<const LinkRecords*>(_ipc());
        for (const auto& link : std::span{page.records, page.count}) if (link.kind == LINK_VM) {
            for (const auto& [area, perms] : std::array<std::pair<_vm_area*, word_t>, 3>{{
                {&*readonly, VM_READ}, {&*writable, VM_READ | VM_WRITE}, {&*staging, VM_READ | VM_WRITE}}})
                if (link.first == area->address) { if (link.perms != perms) return STATUS_INTERNAL; ++found; }
        }
        cursor = page.cursor; epoch = page.epoch;
    } while (cursor);
    if (found != 3 || _vm_area::create(env.alloc(), _vm(), 0, 4096, VM_READ, VM_READ | VM_WRITE)) return STATUS_INTERNAL;
    return STATUS_OK;
}
static auto worker(std::string_view role) -> status_t {
    const auto& env = _startup();
    ubus_conn parent;
    auto status = parent.bind(env.bootstrap);
    if (status != STATUS_OK) return status;
    ubus_msg setup;
    status = parent.invoke("Receive", {}, setup);
    if (status != STATUS_OK) return status;
    if (role == "cycle") {
        ubus_msg request, response; request.data.put("name", "other");
        return parent.invoke("Get", std::move(request), response);
    }
    if (role == "client") {
        if (setup.count != 2) return STATUS_BAD_RIGHTS; // Recipe-only authority must not escape through Receive.
        const auto signal = setup.capability(setup.data["message"]["signal"]);
        if (!signal) return STATUS_INVALID_CAP;
        const auto info = _cap_copy<CapInfo>(_cap_info(signal));
        if (!info || info->view.rights != RIGHT_SIGNAL) return STATUS_BAD_RIGHTS;
        status = _notification_signal(signal).status;
        if (status != STATUS_OK) return status;
        ubus_msg request, response;
        request.data.put("bytes", uint64_t{5 * 1024 * 1024}).put("caps", uint64_t{300});
        status = parent.invoke("Resource", std::move(request), response);
        if (status != STATUS_OK) return status;
        status = _quota_info(env.quota).status;
        if (status != STATUS_OK || reinterpret_cast<const QuotaInfo*>(_ipc())->memory_limit != 5 * 1024 * 1024) return STATUS_INTERNAL;
        ubus_conn concurrent; status = concurrent.bind(env.bootstrap); if (status != STATUS_OK) return status;
        request = {}; request.data.put("name", "calculator");
        status = parent.begin("Get", std::move(request)); if (status != STATUS_OK) return status;
        request = {}; request.data.put("name", "calculator");
        status = concurrent.begin("Get", std::move(request)); if (status != STATUS_OK) return status;
        status = _notification_signal(setup.capability(setup.data["message"]["gate"])).status;
        if (status != STATUS_OK) return status;
        while ((status = parent.poll(response)) == STATUS_WOULD_BLOCK) if (const auto rc = _ev_wait(&_events()).status; rc != STATUS_OK) return rc;
        if (status != STATUS_OK || response.status != STATUS_OK) return status != STATUS_OK ? status : response.status;
        const auto instance = response.data["instance"].as<uint64_t>();
        ubus_msg second;
        while ((status = concurrent.poll(second)) == STATUS_WOULD_BLOCK) if (const auto rc = _ev_wait(&_events()).status; rc != STATUS_OK) return rc;
        if (status != STATUS_OK || second.status != STATUS_OK || second.data["instance"].as<uint64_t>() != instance) return STATUS_INTERNAL;
        ubus_conn calculator;
        status = calculator.bind(response.capability(response.data["connector"]), "fixture.unknown");
        if (status != STATUS_OK) return status;
        request = {}; request.data.put("x", uint64_t{13}).put("y", uint64_t{29});
        status = calculator.invoke("Sum", std::move(request), response);
        if (status != STATUS_OK || response.data["value"].as<uint64_t>() != 42) return STATUS_INTERNAL;
        request = {}; request.data.put("action", "lend").put("bytes", uint64_t{64 * 1024}).put("caps", uint64_t{0})
            .put("max_bytes", uint64_t{128 * 1024}).put("kinds", word_t{OBJ_BIT(ObjChannel) | OBJ_BIT(ObjMem)});
        status = parent.invoke("Resource", std::move(request), response); if (status != STATUS_OK) return status;
        const auto number = *response.data["loan"].as<uint64_t>();
        auto quota = response.take(response.data["quota"]);
        const auto grant = _cap_copy<CapInfo>(_cap_info(quota.selector()));
        if (!grant || grant->view.rights != (RIGHT_CREATE | RIGHT_INSPECT | RIGHT_DUPLICATE)) return STATUS_BAD_RIGHTS;
        request = {};
        const auto quota_idx = request.attach(quota.selector(), grant->view.rights);
        if (!quota_idx) return quota_idx.error();
        request.data.cap("quota", *quota_idx);
        status = calculator.invoke("Borrow", std::move(request), response); if (status != STATUS_OK) return status;
        ubus_conn borrowed; status = borrowed.adopt(response.take(response.data["handle"])); if (status != STATUS_OK) return status;
        status = _quota_info(quota.selector()).status; if (status != STATUS_OK) return status;
        const auto first = *reinterpret_cast<const QuotaInfo*>(_ipc());
        if (first.memory_available >= first.memory_limit) return STATUS_INTERNAL;
        request = {}; request.data.put("loan", number).put("bytes", uint64_t{128 * 1024}).put("caps", uint64_t{0});
        status = parent.invoke("Resource", std::move(request), response); if (status != STATUS_OK) return status;
        status = _quota_info(quota.selector()).status; if (status != STATUS_OK) return status;
        const auto resized = *reinterpret_cast<const QuotaInfo*>(_ipc());
        if (resized.memory_limit != 128 * 1024 || resized.memory_available != first.memory_available + 64 * 1024) return STATUS_INTERNAL;
        status = borrowed.invoke("Ping", {}, response);
        if (status != STATUS_OK || response.data["value"].as<uint64_t>() != 42) return STATUS_INTERNAL;
        // No explicit Return: owner must close the budget after this execution
        // exits, while the live provider drains its independently scoped state.
        return STATUS_OK;
    }
    const auto gate = setup.capability(setup.data["message"]["gate"]);
    status = _wait_one(gate, SOURCE_READY).status; if (status != STATUS_OK) return status;
    if (_notification_take(gate).status != STATUS_OK) return STATUS_INTERNAL;
    // A root has one native lifetime boundary, independent of business names.
    ubus_object old, root;
    for (auto* object : {&old, &root}) {
        const auto scope = _scope_create(env.alloc()); if (scope.status != STATUS_OK) return scope.status;
        status = object->open(1, 0, _cap_owner{scope.value}); if (status != STATUS_OK) return status;
    }
    auto view = old.connector(1), control = old.connector(3), replacement = root.connector(1);
    if (!view || !control || !replacement) return STATUS_INTERNAL;
    const auto attach = [&](uint64_t version, cap_t read, cap_t write, cap_t scope) {
        ubus_msg q, r;
        const auto observe_idx = q.attach(read, RIGHT_SEND | RIGHT_DUPLICATE);
        if (!observe_idx) return observe_idx.error();
        const auto scope_idx = q.attach(scope, RIGHT_INSPECT | RIGHT_DUPLICATE);
        if (!scope_idx) return scope_idx.error();
        q.data.put("version", version).cap("observe", *observe_idx)
            .cap("scope", *scope_idx);
        if (write) {
            const auto manage_idx = q.attach(write, RIGHT_SEND | RIGHT_DUPLICATE);
            if (!manage_idx) return manage_idx.error();
            q.data.cap("manage", *manage_idx);
        }
        return parent.invoke("Attach", std::move(q), r);
    };
    if (attach(0, view->selector(), control->selector(), root.scope()) != STATUS_NOT_FOUND) return STATUS_INTERNAL;
    if (attach(0, view->selector(), control->selector(), old.scope()) != STATUS_OK) return STATUS_INTERNAL;
    if (attach(1, replacement->selector(), 0, root.scope()) != STATUS_BUSY) return STATUS_INTERNAL;
    const auto retained = _cap_duplicate(old.scope(), env.cspace, RIGHT_INSPECT | RIGHT_DUPLICATE);
    if (retained.status != STATUS_OK) return retained.status;
    _cap_owner boundary{retained.value};
    if (old.close() != STATUS_OK) return STATUS_INTERNAL;
    status = _wait_one(boundary.selector(), STATE_DRAINED).status; if (status != STATUS_OK) return status;
    if (attach(0, replacement->selector(), 0, root.scope()) != STATUS_STALE) return STATUS_INTERNAL;
    if (attach(1, replacement->selector(), 0, root.scope()) != STATUS_OK) return STATUS_INTERNAL;
    ubus_object object{"fixture.unknown"};
    std::unique_ptr<ubus_object> borrowed;
    _vm_area funded;
    _cap_owner funded_quota;
    _ev_watch allocation_closing;
    status = object.add("Sum", {{"x", ubus_type::U64}, {"y", ubus_type::U64}}, {{"value", ubus_type::U64}},
        [](auto& q, auto& r) { r.data.put("value", *q.data["x"].template as<uint64_t>() + *q.data["y"].template as<uint64_t>()); return STATUS_OK; });
    if (status == STATUS_OK) status = object.add("Borrow", {{"quota", ubus_type::Cap}}, {{"handle", ubus_type::Cap}}, [&](auto& q, auto& r) {
        const auto quota = q.capability(q.data["quota"]);
        const auto copied = _cap_duplicate(quota, env.cspace, RIGHT_INSPECT);
        if (copied.status != STATUS_OK) return copied.status;
        funded_quota = _cap_owner{copied.value};
        const auto watched = allocation_closing.open(&_events(), quota, STATE_CLOSING | STATE_DRAINED);
        if (watched != STATUS_OK) return watched;
        auto memory = _vm_area::create({quota, env.scope}, _vm(), 0, 4096); if (!memory) return memory.error();
        funded = std::move(*memory); *reinterpret_cast<uint64_t*>(funded.address) = 42;
        borrowed = std::make_unique<ubus_object>();
        auto status = borrowed->add("Ping", {}, {{"value", ubus_type::U64}}, [&](auto&, auto& result) {
            result.data.put("value", *reinterpret_cast<const uint64_t*>(funded.address)); return STATUS_OK;
        });
        if (status == STATUS_OK) status = borrowed->open(4, quota);
        if (status != STATUS_OK) return status;
        auto handle = borrowed->handle(0, RIGHT_SEND); if (!handle) return handle.error();
        const auto handle_idx = r.give(std::move(*handle));
        if (!handle_idx) return handle_idx.error();
        r.data.cap("handle", *handle_idx); return STATUS_OK;
    });
    if (status == STATUS_OK) status = object.open();
    if (status != STATUS_OK) return status;
    auto connector = object.connector(); if (!connector) return connector.error();
    ubus_msg request, response;
    const auto peer_idx = request.attach(connector->selector(), RIGHT_SEND | RIGHT_DUPLICATE);
    if (!peer_idx) return peer_idx.error();
    request.data.put("name", "probe").cap("connector", *peer_idx);
    status = parent.invoke("Publish", std::move(request), response); if (status != STATUS_OK) return status;
    request = {}; request.data.put("instance", response.data["instance"]);
    status = parent.invoke("Withdraw", std::move(request), response); if (status != STATUS_OK) return status;
    if (attach(2, replacement->selector(), 0, root.scope()) != STATUS_BUSY) return STATUS_INTERNAL;
    request = {};
    const auto publish_idx = request.attach(connector->selector(), RIGHT_SEND | RIGHT_DUPLICATE);
    if (!publish_idx) return publish_idx.error();
    request.data.put("name", "answer").put("interface", "fixture.unknown").put("version", uint64_t{1})
        .cap("connector", *publish_idx);
    status = parent.invoke("Publish", std::move(request), response);
    if (status != STATUS_OK) return status;
    for (;;) {
        status = object.poll();
        if (status != STATUS_OK && status != STATUS_WOULD_BLOCK) return status;
        bool progress = status == STATUS_OK;
        if (borrowed) {
            const auto polled = borrowed->poll();
            if (polled == STATUS_CLOSED || polled == STATUS_PEER_CLOSED) { borrowed.reset(); progress = true; }
            else if (polled != STATUS_OK && polled != STATUS_WOULD_BLOCK) return polled;
            else progress |= polled == STATUS_OK;
        }
        if (funded_quota) {
            const auto state = _source_info(funded_quota.selector());
            if (state.status != STATUS_OK || !(state.value & STATE_OPEN)) {
                funded = {}; (void)allocation_closing.close(); funded_quota = {}; progress = true;
            }
        }
        if (!progress) { status = _ev_wait(&_events()).status; if (status != STATUS_OK) return status; }
    }
}
int main(int argc, char** argv) {
    if (argc > 1) return worker(argv[1]);
    if (const auto status = vm_permissions(); status != STATUS_OK) return status;
    const auto& env = _startup();
    auto firmware = fw_view::open(env); if (!firmware) return firmware.error();
    auto irq = firmware->irq(env); if (!irq) return irq.error();
    auto console = fw_console::open(env, firmware->tree, irq->selector()); if (!console) return console.error();
    const auto copy = _cap_duplicate(console->memory.selector(), env.cspace, RIGHT_MAP);
    if (copy.status != STATUS_OK) return copy.status;
    auto mapping = _vm_area::map(_vm(), _cap_owner{copy.value}, 0, 4096, VM_READ | VM_WRITE);
    if (!mapping) return mapping.error();
    uart_port out{mapping->address + console->offset}; out.reset();
    auto archive_memory = _vm_area::map(_vm(), _root().bundle, 0, (_root().bundle_bytes + 4095) & ~size_t{4095}, VM_READ);
    if (!archive_memory) return archive_memory.error();
    const auto archive = libk::cpio_view::parse({reinterpret_cast<const std::byte*>(archive_memory->address), _root().bundle_bytes});
    if (!archive) return STATUS_BAD_ARGS;
    mgr owner{[&](mgr_child& child) -> std::expected<_proc_image, status_t> {
        const auto file = archive.find(child.program());
        if (!file) return std::unexpected(STATUS_NOT_FOUND);
        return _proc_image{.bytes=file->data};
    }};
    auto status = owner.open(); if (status != STATUS_OK) return status;
    auto rejected_owner = owner.configure(mgr_recipe("rejected", "worker"));
    if (!rejected_owner) return rejected_owner.error();
    auto& rejected = **rejected_owner;
    rejected.auth.alloc.quota = 0;
    status = owner.start(rejected);
    if (status == STATUS_OK || status == STATUS_WOULD_BLOCK || rejected.desired
        || rejected.state != mgr_state::Failed || rejected.result != status) return STATUS_INTERNAL;
    rejected.auth.alloc = env.alloc();
    if (owner.poll() != STATUS_OK || rejected.task || rejected.desired) return STATUS_INTERNAL;
    status = owner.erase(rejected.child_id); if (status != STATUS_OK) return status;
    auto incomplete_recipe = mgr_recipe("incomplete", "worker", {.bytes=4096});
    incomplete_recipe.data.put("restart", true);
    auto incomplete_owner = owner.configure(std::move(incomplete_recipe));
    if (!incomplete_owner) return incomplete_owner.error();
    auto& incomplete = **incomplete_owner;
    const auto failure = owner.start(incomplete);
    if (failure == STATUS_OK || failure == STATUS_WOULD_BLOCK || incomplete.task
        || incomplete.desired || incomplete.state != mgr_state::Failed) return STATUS_INTERNAL;
    auto provider_recipe = mgr_recipe("provider", "worker", {.bytes=4 * 1024 * 1024, .caps=256,
        .slots=256, .pages=36});
    ubus_data provider_args{ubus_type::Array}, exports{ubus_type::Array};
    provider_args.push("provider"); exports.push("answer").push("probe");
    provider_recipe.data.put("argv", provider_args).put("exports", exports);
    auto client_recipe = mgr_recipe("client", "worker", {.bytes=4 * 1024 * 1024, .caps=256,
        .slots=256, .pages=36, .quota_perms=RIGHT_CREATE | RIGHT_INSPECT, .max_bytes=8 * 1024 * 1024, .max_caps=384});
    ubus_data client_args{ubus_type::Array}, routes, route, client_setup, provider_setup;
    client_args.push("client");
    route.put("child", "provider").put("name", "answer"); routes.put("calculator", route);
    client_recipe.data.put("argv", client_args).put("routes", routes);
    const auto notice = _notification_create(env.alloc(), 1); if (notice.status != STATUS_OK) return notice.status;
    _cap_owner signal{notice.value};
    const auto linear = _cap_duplicate(signal.selector(), env.cspace, RIGHT_SIGNAL); if (linear.status != STATUS_OK) return linear.status;
    const auto signal_idx = client_recipe.give(_cap_owner{linear.value});
    if (!signal_idx) return signal_idx.error();
    client_setup.cap("signal", *signal_idx);
    const auto gated = _notification_create(env.alloc(), 1); if (gated.status != STATUS_OK) return gated.status;
    _cap_owner gate{gated.value};
    const auto gate_idx = client_recipe.attach(gate.selector(), RIGHT_SIGNAL);
    if (!gate_idx) return gate_idx.error();
    client_setup.cap("gate", *gate_idx);
    const auto private_idx = client_recipe.attach(env.quota, RIGHT_INSPECT | RIGHT_DUPLICATE);
    if (!private_idx) return private_idx.error();
    client_recipe.data.cap("private", *private_idx).put("setup", client_setup);
    const auto rx_idx = provider_recipe.attach(gate.selector(), RIGHT_RECEIVE);
    if (!rx_idx) return rx_idx.error();
    provider_setup.cap("gate", *rx_idx); provider_recipe.data.put("setup", provider_setup);
    auto provider_owner = owner.configure(std::move(provider_recipe)); if (!provider_owner) return provider_owner.error();
    auto client_owner = owner.configure(std::move(client_recipe)); if (!client_owner) return client_owner.error();
    auto& provider = **provider_owner;
    auto& client = **client_owner;
    status = owner.start(client); if (status != STATUS_OK) return status;
    bool done{}, cycling{};
    mgr_child *cycle_a{}, *cycle_b{};
    for (;;) {
        status = owner.poll(); if (status != STATUS_OK) { out.write("[mgr] failed poll\n"); return status; }
        if (!done && !client.task) {
            if (client.result != STATUS_OK || provider.root_version != 2 || !provider.observe || !provider.run_id || _notification_take(signal.selector()).status != STATUS_OK) {
                (void)libk::fmt::format_to<"[mgr] failed execution: client={} provider-run={} provider-state={} provider-result={}\n">(out, client.result, provider.run_id, mgr_state_name(provider.state), provider.result); return STATUS_INTERNAL;
            }
            status = owner.action(provider, "stop"); if (status != STATUS_OK) return status; done = true;
        }
        if (done && !provider.task && !cycling) {
            for (const auto name : {"cycle-a", "cycle-b"}) {
                auto recipe = mgr_recipe(name, "worker", {.bytes=2 * 1024 * 1024, .caps=192, .slots=192, .pages=28});
                ubus_data args{ubus_type::Array}, exports{ubus_type::Array}, routes, route;
                args.push("cycle"); exports.push("answer");
                route.put("child", std::string_view{name} == "cycle-a" ? "cycle-b" : "cycle-a").put("name", "answer");
                routes.put("other", route);
                recipe.data.put("argv", args).put("exports", exports).put("routes", routes);
                auto child = owner.configure(std::move(recipe)); if (!child) return child.error();
                (std::string_view{name} == "cycle-a" ? cycle_a : cycle_b) = child->get();
            }
            status = owner.start(*cycle_a); if (status != STATUS_OK) return status;
            cycling = true;
        }
        if (cycling && !cycle_a->task && !cycle_b->task && !incomplete.task) {
            if (incomplete.desired || incomplete.state != mgr_state::Failed || incomplete.result != failure) return STATUS_INTERNAL;
            const auto deadline = *_after(std::chrono::seconds{5});
            for (;;) {
                const auto scope = _cap_copy<ScopeInfo>(_scope_info(incomplete.scope.selector()));
                if (!scope) return scope.error();
                if (!scope->members) break;
                if (_deadline_status(deadline) != STATUS_OK) return STATUS_TIMED_OUT;
                _yield();
            }
            // One Get rejects the cycle. Its dependant may observe CLOSED
            // while that execution is stopping, before a final result exists.
            const auto a = cycle_a->result, b = cycle_b->result;
            if ((a != STATUS_DENIED && b != STATUS_DENIED)
                || (a != STATUS_DENIED && a != STATUS_CLOSED)
                || (b != STATUS_DENIED && b != STATUS_CLOSED)) return STATUS_INTERNAL;
            out.write("[mgr] environment, lazy Get, direct RPC, linear Receive, Resource and native drain ok\n");
            out.write("[mgr] cross-branch Quota allocation, growth, exit drain and activation cycle rejection ok\n");
            for (;;) _yield();
        }
        status = _ev_wait(&_events(), owner.deadline()).status;
        if (status != STATUS_OK && status != STATUS_TIMED_OUT) return status;
    }
}
