#include <ubus/ubus.hpp>
#include <posix.hpp>
#include <unistd.h>
#include <libk/fmt.hpp>
#include <source_location>

struct ct_output { void write(const char* data, size_t size) { (void)::write(STDOUT_FILENO, data, size); } };
static void check(bool value, std::source_location at = std::source_location::current()) {
    if (value) return;
    ct_output output;
    (void)libk::fmt::format_to<"[control] failed line {}\n">(output, at.line());
    sys::exit(STATUS_INTERNAL);
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
    request.data.put("id", row["id"]).put("gen", *row["gen"].as<uint64_t>() + uint64_t(stale)).put("action", name);
    return owner.invoke("Action", std::move(request), response);
}
static void stopped(ubus_conn& owner, std::string_view name, std::string_view state = "idle") {
    const auto deadline = *rt_after_ms(2000);
    while (find(owner, name)["state"].as<std::string_view>() != state)
        check(rt_admit(deadline) == STATUS_OK);
}
static auto running(ubus_conn& owner, std::string_view name, uint64_t previous = 0, bool delivered = false) -> ubus_data {
    const auto deadline = *rt_after_ms(2000);
    for (;;) {
        auto row = find(owner, name);
        if (row["state"].as<std::string_view>() == "running" && row["gen"].as<uint64_t>().value_or(0) > previous
            && (!delivered || row["delivered"].as<bool>().value_or(false))) return row;
        check(rt_admit(deadline) == STATUS_OK);
    }
}
int main() {
    if (const auto status = px_init(); status != STATUS_OK) return status;
    const auto& env = rt_startup();
    ubus_conn bus, owner;
    check(bus.bind(env.environment) == STATUS_OK);
    ubus_msg lookup, connected; lookup.data.put("name", "session");
    check(bus.invoke("Get", std::move(lookup), connected) == STATUS_OK);
    check(owner.bind(connected.capability(connected.data["connector"])) == STATUS_OK);
    const auto shell = find(owner, "shell");
    check(shell["runtime"].as<uint64_t>().has_value());
    check(shell["charged"].as<uint64_t>().value_or(0) > 0);
    check(shell["resident"].as<uint64_t>().value_or(0) > 0);
    check(action(owner, shell.view(), "stop", true) == STATUS_STALE);
    ubus_msg query, response;
    query.data.put("id", shell["id"]).put("gen", shell["gen"]).put("facet", "caps");
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
    query.data.put("id", shell["id"]).put("gen", shell["gen"]).put("facet", "caps")
        .put("cursor", uint64_t{1}).put("epoch", UINT64_MAX);
    check(owner.invoke("Inspect", std::move(query), response) == STATUS_STALE);

    // This service belongs to session, not to this temporary shell job.
    ubus_data args{ubus_type::Array}; args.push("10000");
    query = {};
    query.data.put("name", "control-sleep").put("program", "/boot/sleep.elf").put("argv", args);
    check(owner.invoke("Spawn", std::move(query), response) == STATUS_OK);
    const auto child = running(owner, "control-sleep");
    ubus_conn watcher; check(watcher.bind(connected.capability(connected.data["connector"])) == STATUS_OK);
    query = {}; query.data.put("id", child["id"]).put("gen", child["gen"]).put("epoch", uint64_t{0});
    check(watcher.invoke("Changed", std::move(query), response) == STATUS_OK);
    const auto epoch = *response.data["epoch"].as<uint64_t>();
    query = {}; query.data.put("id", child["id"]).put("gen", child["gen"]).put("epoch", epoch);
    query.deadline = *rt_after_ms(2000);
    check(watcher.begin("Changed", std::move(query)) == STATUS_OK);
    check(action(owner, child.view(), "stop") == STATUS_OK);
    for (;;) {
        const auto status = watcher.poll(response);
        if (status == STATUS_OK) {
            if (response.status == STATUS_CLOSED) break;
            check(response.status == STATUS_OK && response.data["epoch"].as<uint64_t>() != epoch);
            query = {}; query.data.put("id", child["id"]).put("gen", child["gen"])
                .put("epoch", response.data["epoch"]); query.deadline = *rt_after_ms(2000);
            const auto next = watcher.begin("Changed", std::move(query)); check(next == STATUS_OK);
            continue;
        }
        check(status == STATUS_WOULD_BLOCK); check(ev_wait(env.events, *rt_after_ms(2000)).status == STATUS_OK);
    }
    stopped(owner, "control-sleep");
    check(action(owner, child.view(), "start") == STATUS_OK);
    const auto replaced = running(owner, "control-sleep", *child["gen"].as<uint64_t>());
    check(*replaced["gen"].as<uint64_t>() > *child["gen"].as<uint64_t>());
    check(action(owner, child.view(), "stop") == STATUS_STALE);
    check(action(owner, replaced.view(), "stop") == STATUS_OK);
    stopped(owner, "control-sleep");
    ch_pair channel; check(channel.open(env.alloc(), 1, 0) == STATUS_OK);
    const auto minted = sys::channel_mint(channel.ends[1].selector(), env.cspace, 1, RIGHT_SEND);
    check(minted.status == STATUS_OK);
    cap_owner sender{minted.value};
    check(channel.ends[1].close() == STATUS_OK);
    query = {}; ubus_data setup;
    setup.cap("unused", *query.give(std::move(sender)));
    ch_pair revocable; check(revocable.open(env.alloc(), 1, 0) == STATUS_OK);
    const auto branch = sys::channel_mint(revocable.ends[1].selector(), env.cspace, 1, RIGHT_SEND | RIGHT_DUPLICATE | RIGHT_REVOKE);
    check(branch.status == STATUS_OK); cap_owner branch_cap{branch.value};
    check(revocable.ends[1].close() == STATUS_OK);
    setup.cap("revocable", *query.attach(branch.value, RIGHT_SEND | RIGHT_DUPLICATE | RIGHT_REVOKE));
    ubus_data hold{ubus_type::Array}; hold.push("hold");
    query.data.put("name", "linear-sleep").put("program", "/boot/supervisor.elf").put("argv", hold).put("setup", setup);
    check(owner.invoke("Spawn", std::move(query), response) == STATUS_OK);
    const auto linear = running(owner, "linear-sleep", 0, true);
    query = {}; query.data.put("id", linear["id"]).put("gen", linear["gen"]).put("facet", "caps");
    check(owner.invoke("Inspect", std::move(query), response) == STATUS_OK);
    cap_t selector{};
    for (const auto cap : response.data["items"].values())
        if (cap["kind"].as<uint64_t>() == ObjChannel
            && cap["rights"].as<uint64_t>() == (RIGHT_SEND | RIGHT_DUPLICATE | RIGHT_REVOKE))
            selector = *cap["selector"].as<cap_t>();
    check(selector != 0);
    query = {}; query.data.put("id", linear["id"]).put("gen", linear["gen"]).put("selector", selector);
    check(owner.invoke("Revoke", std::move(query), response) == STATUS_OK);
    check(response.data["state"].as<uint64_t>().value_or(0) & (STATE_CLOSING | STATE_DRAINED));
    check(sys::wait_one(revocable.ends[0].selector(), SOURCE_TERMINAL, 0, *rt_after_ms(1000), 0, SOURCE_READ).status == STATUS_OK);
    const auto source = sys::source_info(channel.ends[0].selector(), SOURCE_READ);
    check(source.status == STATUS_OK && !(source.value & SOURCE_TERMINAL));
    check(action(owner, linear.view(), "stop") == STATUS_OK);
    stopped(owner, "linear-sleep");
    check(sys::wait_one(channel.ends[0].selector(), SOURCE_TERMINAL, 0, *rt_after_ms(1000), 0, SOURCE_READ).status == STATUS_OK);

    // The failed supervisor leaves a running descendant holding the last Send
    // capability. Reusing the same budget proves retirement, not just exit.
    for (const auto name : {"subtree-a", "subtree-b"}) {
        ch_pair proof; check(proof.open(env.alloc(), 1, 0) == STATUS_OK);
        const auto cap = sys::channel_mint(proof.ends[1].selector(), env.cspace, 1, RIGHT_SEND);
        check(cap.status == STATUS_OK); check(proof.ends[1].close() == STATUS_OK);
        query = {}; setup = {};
        setup.cap("proof", *query.give(cap_owner{cap.value}));
        const auto created = sys::notification_create(env.alloc(), 1); check(created.status == STATUS_OK);
        cap_owner crash{created.value};
        setup.cap("crash", *query.attach(crash.selector(), RIGHT_RECEIVE | RIGHT_DUPLICATE));
        ubus_data resources; resources.put("children", true);
        query.data.put("name", name).put("program", "/boot/supervisor.elf").put("setup", setup).put("resources", resources);
        ubus_data routes, upstream; upstream.put("parent", true); routes.put("*", upstream); query.data.put("routes", routes);
        check(owner.invoke("Spawn", std::move(query), response) == STATUS_OK);
        check(sys::wait_one(proof.ends[0].selector(), SOURCE_READY, 0, *rt_after_ms(2000), 0, SOURCE_READ).status == STATUS_OK);
        ch_packet message; const auto received = ch_recv(proof.ends[0].selector(), message, false).status;
        if (received != STATUS_OK) {
            stopped(owner, name, "failed");
            const auto row = find(owner, name);
            ct_output output;
            (void)libk::fmt::format_to<"[control] subtree receive={} exit={}\n">(output, received, row["result"].as<int64_t>().value_or(0));
        }
        check(received == STATUS_OK);
        check(message.word_count == 1 && message.words[0] == 42);
        check(!(sys::source_info(proof.ends[0].selector(), SOURCE_READ).value & SOURCE_TERMINAL));
        check(sys::notification_signal(crash.selector()).status == STATUS_OK);
        check(sys::wait_one(proof.ends[0].selector(), SOURCE_TERMINAL, 0, *rt_after_ms(2000), 0, SOURCE_READ).status == STATUS_OK);
        stopped(owner, name, "failed");
        check(find(owner, name)["result"].as<int64_t>() == STATUS_INTERNAL);
    }
    ct_output output;
    constexpr std::string_view marker{"[control] scoped badges, stale instances/cursors and real VM/account stats ok\n"};
    output.write(marker.data(), marker.size());
    return STATUS_OK;
}
