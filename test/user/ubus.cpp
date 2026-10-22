#include <ubus/ubus.hpp>
#include "task.hpp"
#include "../../apps/uart/uart.hpp"
#include "../../apps/devices/fw.hpp"
#include <source_location>

static uart_port* diagnostic;
static void check(bool value, std::source_location location = std::source_location::current()) {
    if (!value) {
        if (diagnostic) (void)libk::fmt::format_to<"[ubus] check failed line={}\n">(*diagnostic, location.line());
        sys::exit(-static_cast<status_t>(location.line()));
    }
}
static void expect(status_t status, std::source_location location = std::source_location::current()) {
    if (status != STATUS_OK) {
        if (diagnostic) (void)libk::fmt::format_to<"[ubus] failure line={} status={}\n">(*diagnostic, location.line(), status);
        sys::exit(-static_cast<status_t>(location.line() * 100) + status);
    }
}
static auto provider() -> int {
    ubus_object object{"example.calculator", 3};
    bool done{};
    std::optional<ubus_request> delayed;
    ubus_conn environment; expect(environment.bind(rt_startup().environment));
    ubus_msg setup; expect(environment.invoke("Receive", {}, setup));
    const auto destination = setup.capability(setup.data["message"]["handoff"]);
    expect(object.add("sum", {{"a", ubus_type::U64}, {"b", ubus_type::U64}}, {{"value", ubus_type::U64}},
        [](auto& request, auto& result) {
            result.data.put("value", *request.data["a"].template as<uint64_t>() + *request.data["b"].template as<uint64_t>());
            return STATUS_OK;
        }));
    expect(object.add("echo", {{"text", ubus_type::String}, {"samples", ubus_type::Vector, true}},
        {{"text", ubus_type::String}, {"samples", ubus_type::Vector, true}}, [](auto& request, auto& result) {
            result.data.put("text", request.data["text"]);
            if (request.data["samples"]) result.data.put("samples", request.data["samples"]);
            return STATUS_OK;
        }));
    expect(object.add("defer", {{"text", ubus_type::String}}, {{"text", ubus_type::String}},
        [&](auto& request, auto&) { delayed.emplace(std::move(request)); return STATUS_PENDING; }));
    expect(object.add("finish", {}, {}, [&](auto&, auto&) {
        if (delayed) {
            ubus_msg result; result.data.put("text", delayed->data["text"]);
            const auto status = delayed->respond(std::move(result));
            check(status == STATUS_OK || status == STATUS_CANCELED || status == STATUS_STALE);
            delayed.reset();
        }
        return STATUS_OK;
    }));
    expect(object.add("signal", {{"notice", ubus_type::Cap}}, {}, [](auto& request, auto&) {
        return sys::notification_signal(request.capability(request.data["notice"])).status;
    }));
    expect(object.add("stop", {}, {}, [&](auto&, auto&) { done = true; return STATUS_OK; }));
    expect(object.add("admin", {}, {}, [](auto&, auto&) { return STATUS_OK; }, 1));
    expect(object.add("notify", {{"notice", ubus_type::Cap}, {"text", ubus_type::String, false, 16000}}, {}, [](auto& request, auto&) {
        if (request.data["text"].template as<std::string_view>()->size() != 16000) return STATUS_BAD_ARGS;
        return sys::notification_signal(request.capability(request.data["notice"])).status;
    }, 0, true));
    const ubus_field samples_field{"samples", ubus_type::Vector, true, UINT64_MAX, {}, ubus_atom::U16};
    expect(object.add("shape", {
        {"options", ubus_type::Record, false, UINT64_MAX, {{"limit", ubus_type::U64}, samples_field}},
        {"values", ubus_type::Array, false, UINT64_MAX, {{"", ubus_type::Record, false, UINT64_MAX, {{"id", ubus_type::U64}}}}},
        {"none", ubus_type::Null}},
        {{"accepted", ubus_type::Bool}}, [](auto&, auto& result) { result.data.put("accepted", true); return STATUS_OK; }));
    expect(object.open());
    auto connector = object.connector(); check(connector.has_value());
    check(object.add("late", {}, {}, [](auto&, auto&) { return STATUS_OK; }) == STATUS_BUSY);
    ubus_msg ready;
    const auto peer_idx = ready.offer(std::move(*connector), RIGHT_SEND | RIGHT_DUPLICATE | RIGHT_REVOKE);
    if (!peer_idx) return peer_idx.error();
    ready.data.cap("connector", *peer_idx);
    expect(ubus_send(destination, ready));
    while (!done) {
        const auto status = object.poll();
        if (status == STATUS_WOULD_BLOCK) expect(ev_wait(rt_startup().events).status);
        else expect(status);
    }
    return STATUS_OK;
}
int main(int argc, char**) {
    if (argc > 1) return provider();
    const auto& env = rt_startup();
    auto firmware = fw_view::open(env); check(firmware.has_value());
    auto irq = firmware->irq(env); check(irq.has_value());
    auto console = fw_console::open(env, firmware->tree, irq->selector()); check(console.has_value());
    const auto mapped = sys::cap_duplicate(console->memory.selector(), env.cspace, RIGHT_MAP); expect(mapped.status);
    auto mapping = vm_area::map(rt_vm(), cap_owner{mapped.value}, 0, 4096, VM_READ | VM_WRITE); check(mapping.has_value());
    uart_port output{mapping->address + console->offset}; output.reset(); diagnostic = &output;
    test_image image; open(image, env);
    test_tasks tasks; tasks.open(env.alloc(), rt_root().domain, env.cpus);
    ch_pair handoff; expect(handoff.open(env.alloc(), 1, 2));
    const auto minted = sys::channel_mint(handoff.ends[1].selector(), env.cspace, 1, RIGHT_SEND | RIGHT_DUPLICATE);
    expect(minted.status); cap_owner sender{minted.value};
    {
        ubus_msg full;
        for (size_t i = 0; i < CHANNEL_MAX_CAPS; ++i) check(full.attach(sender.selector(), RIGHT_SEND).has_value());
        const auto kept = sys::cap_duplicate(sender.selector(), env.cspace, RIGHT_SEND);
        expect(kept.status); cap_owner owned{kept.value};
        const auto failed = full.give(std::move(owned));
        check(!failed && failed.error() == STATUS_NO_MEMORY && owned.selector() == kept.value);
        check(cap_info(owned.selector()).has_value());
    }
    ubus_object environment{"fixture"};
    expect(environment.add("Receive", {}, {{"message", ubus_type::Record}}, [&](auto&, auto& response) {
        ubus_data message;
        const auto handoff_idx = response.attach(sender.selector(), RIGHT_SEND);
        if (!handoff_idx) return handoff_idx.error();
        message.cap("handoff", *handoff_idx);
        response.data.put("message", message); return STATUS_OK;
    }));
    expect(environment.open()); auto endpoint = environment.connector(); check(bool(endpoint));
    const std::string_view argv[]{"provider", "--provider"};
    const auto child = start(tasks, image, "provider", {.argv=argv, .environment=endpoint->selector(),
        .resources={.bytes=8 * 1024 * 1024, .caps=512, .slots=512, .pages=68}});
    ubus_rx handoff_rx; expect(handoff_rx.open(handoff.ends[0].selector(), env.events));
    ubus_msg ready;
    for (;;) {
        const auto status = handoff_rx.receive(ready);
        if (status == STATUS_OK) break;
        check(status == STATUS_WOULD_BLOCK);
        const auto received = environment.poll();
        if (received == STATUS_WOULD_BLOCK) expect(ev_wait(env.events).status); else expect(received);
    }
    auto connector = ready.take(ready.data["connector"]); check(bool(connector));
    ubus_conn connection;
    check(connection.bind(connector.selector(), "wrong") == STATUS_BAD_ARGS);
    expect(connection.bind(connector.selector(), "example.calculator", 2, 3));
    check(!connection.token("admin") && connection.token("sum").has_value());
    check(connection.description()["version"].as<uint64_t>() == 3);
    ubus_msg request, response;
    request.data.put("a", uint64_t{17}).put("b", uint64_t{25});
    expect(connection.invoke("sum", std::move(request), response));
    check(response.data["value"].as<uint64_t>() == 42);
    request = {}; request.data.put("a", "invalid").put("b", uint64_t{25});
    check(connection.invoke("sum", std::move(request), response) == STATUS_BAD_ARGS);
    ubus_call raw; request = {}; request.tag = 8;
    expect(raw.begin(connection.endpoint(), std::move(request))); expect(raw.wait(response));
    check(response.status == STATUS_BAD_RIGHTS);
    const std::string large(16000, 'x');
    const std::array<uint32_t, 5> samples{1, 2, 3, 0, UINT32_MAX};
    request = {}; request.data.put("text", large).array("samples", std::span<const uint32_t>{samples});
    expect(connection.invoke("echo", std::move(request), response));
    check(response.data["text"].as<std::string_view>() == large);
    const auto returned = response.data["samples"].vector<uint32_t>();
    check(returned && std::ranges::equal(*returned, samples));
    ubus_data options, values{ubus_type::Array}, item;
    const std::array<uint16_t, 2> weights{1, 2};
    options.put("limit", uint64_t{7}).array("samples", std::span{weights});
    item.put("id", uint64_t{1}); values.push(item);
    request = {}; request.data.put("options", options).put("values", values).put("none", nullptr);
    expect(connection.invoke("shape", std::move(request), response));
    check(response.data["accepted"].as<bool>().value_or(false));
    options = {}; options.put("limit", uint64_t{7}).array("samples", std::span{samples});
    request = {}; request.data.put("options", options).put("values", values).put("none", nullptr);
    check(connection.invoke("shape", std::move(request), response) == STATUS_BAD_ARGS);
    options = {}; options.put("limit", uint64_t{7}).array("samples", std::span{weights}).put("unknown", true);
    request = {}; request.data.put("options", options).put("values", values).put("none", nullptr);
    check(connection.invoke("shape", std::move(request), response) == STATUS_BAD_ARGS);
    auto notice = sys::notification_create(env.alloc(), 7); expect(notice.status);
    cap_owner notice_owner{notice.value};
    request = {};
    const auto notice_idx = request.attach(notice.value, RIGHT_SIGNAL);
    if (!notice_idx) return notice_idx.error();
    request.data.cap("notice", *notice_idx);
    expect(connection.invoke("signal", std::move(request), response));
    check(sys::notification_take(notice.value).value == 7);
    request = {};
    const auto cancel_idx = request.attach(notice.value, RIGHT_SIGNAL);
    if (!cancel_idx) return cancel_idx.error();
    request.data.cap("notice", *cancel_idx);
    request.data.put("text", large);
    expect(connection.notify("notify", std::move(request)));
    expect(sys::wait_one(notice.value, 7).status);
    check(sys::notification_take(notice.value).value == 7);
    request = {}; request.data.put("text", large);
    expect(connection.begin("defer", std::move(request)));
    // A second connection triggers completion after the first request's stack
    // frame and raw packet have gone away, exercising owned deferred views.
    ubus_conn other; expect(other.bind(connector.selector()));
    expect(other.invoke("finish", {}, response));
    while (connection.poll(response) == STATUS_WOULD_BLOCK) expect(ev_wait(env.events).status);
    check(response.data["text"].as<std::string_view>() == large);
    request = {}; request.data.put("text", large);
    expect(connection.begin("defer", std::move(request)));
    // Bind is processed after defer on the same inbound queue, so cancel races
    // an actually accepted deferred request rather than an unsent local call.
    ubus_conn barrier; expect(barrier.bind(connector.selector()));
    expect(connection.cancel());
    expect(other.invoke("finish", {}, response));
    request = {}; request.data.put("a", uint64_t{1}).put("b", uint64_t{2});
    expect(connection.invoke("sum", std::move(request), response));
    check(response.data["value"].as<uint64_t>() == 3);
    expect(other.invoke("stop", {}, response));
    expect(wait(tasks, child));
    output.write("[ubus] dynamic methods, facets, capabilities, bulk and deferred views ok\n");
    sys::exit();
}
