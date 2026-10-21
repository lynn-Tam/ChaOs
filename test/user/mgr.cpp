#include <mgr/mgr.hpp>
#include "../../apps/devices/fw.hpp"
#include "../../apps/uart/uart.hpp"
#include <libk/fmt.hpp>

static auto worker(std::string_view role) -> status_t {
    const auto& env = rt_startup();
    ubus_conn parent;
    auto status = parent.bind(env.environment);
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
        const auto info = cap_info(signal);
        if (!info || info->view.rights != RIGHT_SIGNAL) return STATUS_BAD_RIGHTS;
        status = sys::notification_signal(signal).status;
        if (status != STATUS_OK) return status;
        ubus_msg request, response;
        request.data.put("bytes", uint64_t{5 * 1024 * 1024}).put("caps", uint64_t{300});
        status = parent.invoke("Resource", std::move(request), response);
        if (status != STATUS_OK) return status;
        status = sys::account_info(env.pool).status;
        if (status != STATUS_OK || reinterpret_cast<const AccountInfo*>(env.ipc)->memory_limit != 5 * 1024 * 1024) return STATUS_INTERNAL;
        ubus_conn concurrent; status = concurrent.bind(env.environment); if (status != STATUS_OK) return status;
        request = {}; request.data.put("name", "calculator");
        status = parent.begin("Get", std::move(request)); if (status != STATUS_OK) return status;
        request = {}; request.data.put("name", "calculator");
        status = concurrent.begin("Get", std::move(request)); if (status != STATUS_OK) return status;
        status = sys::notification_signal(setup.capability(setup.data["message"]["gate"])).status;
        if (status != STATUS_OK) return status;
        while ((status = parent.poll(response)) == STATUS_WOULD_BLOCK) if (const auto rc = ev_wait(env.events).status; rc != STATUS_OK) return rc;
        if (status != STATUS_OK || response.status != STATUS_OK) return status != STATUS_OK ? status : response.status;
        const auto instance = response.data["instance"].as<uint64_t>();
        ubus_msg second;
        while ((status = concurrent.poll(second)) == STATUS_WOULD_BLOCK) if (const auto rc = ev_wait(env.events).status; rc != STATUS_OK) return rc;
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
        auto account = response.take(response.data["account"]);
        const auto grant = cap_info(account.selector());
        if (!grant || grant->view.rights != (RIGHT_CREATE | RIGHT_INSPECT | RIGHT_DUPLICATE)) return STATUS_BAD_RIGHTS;
        request = {}; request.data.cap("account", *request.attach(account.selector(), grant->view.rights));
        status = calculator.invoke("Borrow", std::move(request), response); if (status != STATUS_OK) return status;
        ubus_conn borrowed; status = borrowed.adopt(response.take(response.data["handle"])); if (status != STATUS_OK) return status;
        status = sys::account_info(account.selector()).status; if (status != STATUS_OK) return status;
        const auto first = *reinterpret_cast<const AccountInfo*>(env.ipc);
        if (first.memory_available >= first.memory_limit) return STATUS_INTERNAL;
        request = {}; request.data.put("loan", number).put("bytes", uint64_t{128 * 1024}).put("caps", uint64_t{0});
        status = parent.invoke("Resource", std::move(request), response); if (status != STATUS_OK) return status;
        status = sys::account_info(account.selector()).status; if (status != STATUS_OK) return status;
        const auto resized = *reinterpret_cast<const AccountInfo*>(env.ipc);
        if (resized.memory_limit != 128 * 1024 || resized.memory_available != first.memory_available + 64 * 1024) return STATUS_INTERNAL;
        status = borrowed.invoke("Ping", {}, response);
        if (status != STATUS_OK || response.data["value"].as<uint64_t>() != 42) return STATUS_INTERNAL;
        // No explicit Return: owner must close the budget after this execution
        // exits, while the live provider drains its independently scoped state.
        sys::exit(STATUS_OK);
    }
    const auto gate = setup.capability(setup.data["message"]["gate"]);
    status = sys::wait_one(gate, SOURCE_READY).status; if (status != STATUS_OK) return status;
    if (sys::notification_take(gate).status != STATUS_OK) return STATUS_INTERNAL;
    ubus_object object{"fixture.unknown"};
    std::unique_ptr<ubus_object> borrowed;
    vm_area funded;
    cap_owner funded_account;
    ev_watch funding_closing;
    status = object.add("Sum", {{"x", ubus_type::U64}, {"y", ubus_type::U64}}, {{"value", ubus_type::U64}},
        [](auto& q, auto& r) { r.data.put("value", *q.data["x"].template as<uint64_t>() + *q.data["y"].template as<uint64_t>()); return STATUS_OK; });
    if (status == STATUS_OK) status = object.add("Borrow", {{"account", ubus_type::Cap}}, {{"handle", ubus_type::Cap}}, [&](auto& q, auto& r) {
        const auto account = q.capability(q.data["account"]);
        const auto copied = sys::cap_duplicate(account, env.cspace, RIGHT_INSPECT);
        if (copied.status != STATUS_OK) return copied.status;
        funded_account = cap_owner{copied.value};
        const auto watched = funding_closing.open(env.events, account, STATE_CLOSING | STATE_DRAINED);
        if (watched != STATUS_OK) return watched;
        auto memory = vm_area::create({account, env.scope}, rt_vm(), 0, 4096); if (!memory) return memory.error();
        funded = std::move(*memory); *reinterpret_cast<uint64_t*>(funded.address) = 42;
        borrowed = std::make_unique<ubus_object>();
        auto status = borrowed->add("Ping", {}, {{"value", ubus_type::U64}}, [&](auto&, auto& result) {
            result.data.put("value", *reinterpret_cast<const uint64_t*>(funded.address)); return STATUS_OK;
        });
        if (status == STATUS_OK) status = borrowed->open(4, account);
        if (status != STATUS_OK) return status;
        auto handle = borrowed->handle(0, RIGHT_SEND); if (!handle) return handle.error();
        r.data.cap("handle", *r.give(std::move(*handle))); return STATUS_OK;
    });
    if (status == STATUS_OK) status = object.open();
    if (status != STATUS_OK) return status;
    auto connector = object.connector(); if (!connector) return connector.error();
    ubus_msg request, response;
    request.data.put("name", "answer").put("interface", "fixture.unknown").put("version", uint64_t{1})
        .cap("connector", *request.attach(connector->selector(), RIGHT_SEND | RIGHT_DUPLICATE));
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
        if (funded_account) {
            const auto state = sys::source_info(funded_account.selector());
            if (state.status != STATUS_OK || !(state.value & STATE_OPEN)) {
                funded = {}; (void)funding_closing.close(); funded_account = {}; progress = true;
            }
        }
        if (!progress) { status = ev_wait(env.events).status; if (status != STATUS_OK) return status; }
    }
}
int main(int argc, char** argv) {
    if (argc > 1) return worker(argv[1]);
    const auto& env = rt_startup();
    auto firmware = fw_view::open(env); if (!firmware) return firmware.error();
    auto irq = firmware->irq(env); if (!irq) return irq.error();
    auto console = fw_console::open(env, firmware->tree, irq->selector()); if (!console) return console.error();
    const auto copy = sys::cap_duplicate(console->memory.selector(), env.cspace, RIGHT_MAP);
    if (copy.status != STATUS_OK) return copy.status;
    auto mapping = vm_area::map(rt_vm(), cap_owner{copy.value}, 0, 4096, VM_READ | VM_WRITE);
    if (!mapping) return mapping.error();
    uart_port out{mapping->address + console->offset}; out.reset();
    proc_archive archive;
    auto status = archive.open(rt_root().bundle, rt_root().bundle_bytes); if (status != STATUS_OK) return status;
    mgr owner{[&](mgr_child& child) { return archive.image(child.image); }};
    status = owner.open(); if (status != STATUS_OK) return status;
    auto& rejected = owner.add("rejected", "worker");
    rejected.auth.alloc.account = 0;
    status = owner.start(rejected);
    if (status == STATUS_OK || status == STATUS_WOULD_BLOCK || rejected.desired
        || rejected.state != "failed" || rejected.result != status) return STATUS_INTERNAL;
    rejected.auth.alloc = env.alloc();
    if (owner.poll() != STATUS_OK || rejected.task || rejected.desired) return STATUS_INTERNAL;
    status = owner.erase(rejected.id); if (status != STATUS_OK) return status;
    auto& incomplete = owner.add("incomplete", "worker", {.bytes=4096});
    incomplete.restart = true;
    const auto failure = owner.start(incomplete);
    if (failure == STATUS_OK || failure == STATUS_WOULD_BLOCK || !incomplete.task
        || incomplete.desired || incomplete.state != "draining") return STATUS_INTERNAL;
    auto& provider = owner.add("provider", "worker", {.bytes=4 * 1024 * 1024, .caps=256,
        .slots=256, .pages=36});
    provider.args.push_back("provider"); provider.exports.push("answer");
    auto& client = owner.add("client", "worker", {.bytes=4 * 1024 * 1024, .caps=256,
        .slots=256, .pages=36, .pool_perms=RIGHT_CREATE | RIGHT_INSPECT, .max_bytes=8 * 1024 * 1024, .max_caps=384});
    client.args.push_back("client");
    ubus_data route; route.put("child", "provider").put("name", "answer"); client.routes.put("calculator", route);
    const auto notice = sys::notification_create(env.alloc(), 1); if (notice.status != STATUS_OK) return notice.status;
    cap_owner signal{notice.value};
    const auto linear = sys::cap_duplicate(signal.selector(), env.cspace, RIGHT_SIGNAL); if (linear.status != STATUS_OK) return linear.status;
    client.setup.data.cap("signal", *client.setup.give(cap_owner{linear.value}));
    const auto gated = sys::notification_create(env.alloc(), 1); if (gated.status != STATUS_OK) return gated.status;
    cap_owner gate{gated.value};
    client.setup.data.cap("gate", *client.setup.attach(gate.selector(), RIGHT_SIGNAL));
    (void)client.setup.attach(env.pool, RIGHT_INSPECT | RIGHT_DUPLICATE); // Owner-private, absent from delivered data.

    provider.setup.data.cap("gate", *provider.setup.attach(gate.selector(), RIGHT_RECEIVE));
    status = owner.start(client); if (status != STATUS_OK) return status;
    bool done{}, cycling{};
    mgr_child *cycle_a{}, *cycle_b{};
    for (;;) {
        status = owner.poll(); if (status != STATUS_OK) { out.write("[mgr] failed poll\n"); return status; }
        if (!done && !client.task) {
            if (client.result != STATUS_OK || !provider.gen || sys::notification_take(signal.selector()).status != STATUS_OK) {
                (void)libk::fmt::format_to<"[mgr] failed execution: client={} provider-gen={} provider-state={} provider-result={}\n">(out, client.result, provider.gen, provider.state, provider.result); return STATUS_INTERNAL;
            }
            status = owner.action(provider, "stop"); if (status != STATUS_OK) return status; done = true;
        }
        if (done && !provider.task && !cycling) {
            cycle_a = &owner.add("cycle-a", "worker", {.bytes=2 * 1024 * 1024, .caps=192, .slots=192, .pages=28});
            cycle_b = &owner.add("cycle-b", "worker", {.bytes=2 * 1024 * 1024, .caps=192, .slots=192, .pages=28});
            for (auto* child : {cycle_a, cycle_b}) { child->args.push_back("cycle"); child->exports.push("answer"); }
            ubus_data left, right;
            left.put("child", "cycle-b").put("name", "answer"); right.put("child", "cycle-a").put("name", "answer");
            cycle_a->routes.put("other", left); cycle_b->routes.put("other", right);
            status = owner.start(*cycle_a); if (status != STATUS_OK) return status;
            cycling = true;
        }
        if (cycling && !cycle_a->task && !cycle_b->task && !incomplete.task) {
            if (incomplete.desired || incomplete.state != "failed" || incomplete.result != failure) return STATUS_INTERNAL;
            if (cycle_a->result != STATUS_DENIED || cycle_b->result != STATUS_DENIED) return STATUS_INTERNAL;
            out.write("[mgr] environment, lazy Get, direct RPC, linear Receive, Resource and native drain ok\n");
            out.write("[mgr] cross-branch Account funding, growth, exit drain and activation cycle rejection ok\n");
            for (;;) sys::yield();
        }
        status = ev_wait(env.events, owner.deadline()).status;
        if (status != STATUS_OK && status != STATUS_TIMED_OUT) return status;
    }
}
