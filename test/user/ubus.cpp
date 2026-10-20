#include <ubus/ubus.hpp>
#include <lib/io/io.hpp>
#include <lib/stream/stream.hpp>
#include <test/user/task.hpp>
#include <servers/uart.hpp>
#include <servers/fw.hpp>
#include <source_location>

static uart_port* diagnostic;
static constexpr rt_key bus_test{"test.bus", ObjChannel};
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
    std::unique_ptr<io_server> file;
    std::optional<io_ticket> held;
    bool release{};
    std::array<uint8_t, 4096> contents{};
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
    expect(object.add("Open", {}, {{"handle", ubus_type::Cap}},
        [&](auto&, auto& result) {
            if (file) return STATUS_BUSY;
            file = std::make_unique<io_server>();
            auto status = file->open(contents.size(), io_read | io_write);
            if (status != STATUS_OK) return status;
            auto connector = file->object().handle(0, RIGHT_SEND);
            if (!connector) return connector.error();
            result.data.cap("handle", *result.give(std::move(*connector)));
            return STATUS_OK;
        }));
    expect(object.add("Release", {}, {}, [&](auto&, auto&) { release = true; return STATUS_OK; }));
    expect(object.add("notify", {{"notice", ubus_type::Cap}, {"text", ubus_type::String, false, 16000}}, {}, [](auto& request, auto&) {
        if (request.data["text"].template as<std::string_view>()->size() != 16000) return STATUS_BAD_ARGS;
        return sys::notification_signal(request.capability(request.data["notice"])).status;
    }, 0, true));
    expect(object.add("State", {}, {{"open", ubus_type::Bool}}, [&](auto&, auto& result) {
        result.data.put("open", bool(file)); return STATUS_OK;
    }));
    const ubus_field samples_field{"samples", ubus_type::Vector, true, UINT64_MAX, {}, ubus_atom::U16};
    expect(object.add("shape", {
        {"options", ubus_type::Record, false, UINT64_MAX, {{"limit", ubus_type::U64}, samples_field}},
        {"values", ubus_type::Array, false, UINT64_MAX, {{"", ubus_type::Record, false, UINT64_MAX, {{"id", ubus_type::U64}}}}},
        {"none", ubus_type::Null}},
        {{"accepted", ubus_type::Bool}}, [](auto&, auto& result) { result.data.put("accepted", true); return STATUS_OK; }));
    expect(object.open());
    auto connector = object.connector(); check(connector.has_value());
    check(object.add("late", {}, {}, [](auto&, auto&) { return STATUS_OK; }) == STATUS_BUSY);
    ubus_msg ready; ready.data.cap("connector", *ready.offer(std::move(*connector), RIGHT_SEND | RIGHT_DUPLICATE | RIGHT_REVOKE));
    expect(ubus_send(rt_startup().selector(bus_test), ready));
    while (!done) {
        const auto status = object.poll();
        if (status == STATUS_WOULD_BLOCK) expect(ev_wait(rt_startup().events).status);
        else expect(status);
        if (file) {
            expect(file->poll());
            io_ticket ticket;
            if (held && release) {
                const auto command = *file->queue()->request(*held);
                ::memcpy(file->payload() + command.buffer_offset, contents.data(), command.length);
                check(file->queue()->finish(*held, STATUS_OK, command.length)); held.reset();
            }
            while (file->queue()->admit(ticket) == io_admission::Ready) {
                const auto command = *file->queue()->request(ticket);
                check(command.offset == 0 && command.length <= contents.size());
                check(file->queue()->commit(ticket));
                if (command.operation == io_operation::Write) {
                    check(file->payload()[command.buffer_offset] == command.id);
                    ::memcpy(contents.data(), file->payload() + command.buffer_offset, command.length);
                    check(file->queue()->finish(ticket, STATUS_OK, command.length));
                } else {
                    held = ticket;
                    ubus_msg admitted; admitted.data.put("admitted", true);
                    expect(ubus_send(rt_startup().selector(bus_test), admitted));
                }
            }
            expect(file->flush(true));
            if (file->done()) file.reset();
        }
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
    ch_pair bootstrap; expect(bootstrap.open(env.alloc(), 2, 2));
    const proc_cap directory_caps[]{
        input(ubus_boot, bootstrap.ends[1].selector(), RIGHT_SEND | RIGHT_RECEIVE, 1),
        input(ubus_owner, env.scope, RIGHT_INSPECT | RIGHT_DUPLICATE)};
    const auto registry = start(tasks, image, "ubusd", {.caps = directory_caps,
        .resources = {.bytes = 8 * 1024 * 1024, .caps = 1024, .kinds = proc_base_kinds | OBJ_BIT(ObjChannel),
                      .slots = 1024, .pages = 132}});
    const auto boot_sender = sys::channel_mint(bootstrap.ends[0].selector(), env.cspace, 1, RIGHT_SEND);
    expect(boot_sender.status); cap_owner boot_cap{boot_sender.value};
    ubus_msg config; config.data.put("quota", uint64_t{128});
    expect(ubus_send(boot_cap.selector(), config));
    ubus_msg registry_ready;
    for (;;) {
        const auto received = ubus_receive_event(bootstrap.ends[0].selector(), registry_ready);
        if (received == STATUS_OK) break;
        check(received == STATUS_WOULD_BLOCK);
        const auto exited = tasks.collect(registry);
        if (exited.status == STATUS_OK) {
            (void)libk::fmt::format_to<"[ubus] registry exited status={}\n">(output, exited.value);
            check(false);
        }
        sys::yield();
    }
    auto root_connector = registry_ready.take(registry_ready.data["connector"]); check(bool(root_connector));
    ubus_conn root_directory; expect(root_directory.bind(root_connector.selector()));
    ch_pair handoff; expect(handoff.open(env.alloc(), 1, 2));
    const proc_cap cap = input(bus_test, handoff.ends[1].selector(), RIGHT_SEND, 1);
    const std::string_view argv[]{"provider", "--provider"};
    const auto child = start(tasks, image, "provider", {.argv = argv, .caps = {&cap, 1},
        .resources = {.bytes = 8 * 1024 * 1024, .caps = 512, .kinds = proc_base_kinds | OBJ_BIT(ObjChannel),
                      .slots = 512, .pages = 68}});
    ubus_msg ready; expect(ubus_receive_event(handoff.ends[0].selector(), ready, true));
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
    request = {}; request.data.cap("notice", *request.attach(notice.value, RIGHT_SIGNAL));
    expect(connection.invoke("signal", std::move(request), response));
    check(sys::notification_take(notice.value).value == 7);
    request = {}; request.data.cap("notice", *request.attach(notice.value, RIGHT_SIGNAL));
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
    {
        request = {};
        expect(connection.invoke("Open", std::move(request), response));
        auto handle = response.take(response.data["handle"]);
        io_client file; expect(file.open(std::move(handle), true));
        std::array<uint8_t, 4096> written, read{};
        for (size_t i = 0; i < written.size(); ++i) written[i] = i % 251 + 1;
        io_cmd write{.operation = io_operation::Write, .length = written.size()};
        expect(file.submit(write, written.data(), true));
        io_done result; expect(file.wait(result, write.id)); expect(result.status);
        // Move an already used linear handle. Its old buffer views are revoked
        // on adoption, and the next caller resumes the accepted sequence.
        ubus_msg old_buffer; expect(file.invoke("Buffer", {}, old_buffer));
        auto previous = old_buffer.take(old_buffer.data["buffer"]); check(bool(previous));
        auto moved = file.release(); check(bool(moved));
        expect(file.open(std::move(*moved), true));
        std::array<std::byte, 1> byte;
        check(mem_read(previous.selector(), 0, byte) != STATUS_OK);
        std::vector<io_cmd> batch(io_queue_depth);
        for (size_t i = 0; i < batch.size(); ++i) {
            written[0] = i + 2;
            batch[i] = {.operation = io_operation::Write, .length = written.size()};
            expect(file.submit(batch[i], written.data(), true));
        }
        io_cmd excess{.operation = io_operation::Write, .length = written.size()};
        check(file.submit(excess, written.data(), true) == STATUS_BUSY);
        for (const auto& item : batch) { expect(file.wait(result, item.id)); expect(result.status); }
        io_cmd command{.operation = io_operation::Read, .length = read.size(),
            .deadline = *rt_after_ms(100)};
        expect(file.submit(command, read.data(), true));
        auto busy = file.release(); check(!busy && busy.error() == STATUS_BUSY);
        ubus_msg admitted; expect(ubus_receive_event(handoff.ends[0].selector(), admitted, true));
        check(admitted.data["admitted"].as<bool>().value_or(false));
        check(file.cancel(command.id) == STATUS_BUSY);
        check(file.wait(result, command.id, command.deadline) == STATUS_TIMED_OUT);
        check(file.contains(command.id) && read[0] == 0);
        expect(connection.invoke("Release", {}, response));
        expect(file.wait(result, command.id)); expect(result.status);
        check(result.bytes == written.size() && read == written);
        expect(file.close());
    }
    expect(connection.invoke("Open", {}, response));
    auto abandoned = response.take(response.data["handle"]); check(bool(abandoned));
    expect(abandoned.close());
    const auto retired = *rt_after_ms(1000);
    for (;;) {
        expect(connection.invoke("State", {}, response));
        if (!response.data["open"].as<bool>().value_or(true)) break;
        expect(rt_admit(retired));
    }
    const auto lease = sys::scope_create(env.alloc()); expect(lease.status);
    cap_owner publish_scope{lease.value};
    request = {}; request.data.put("prefix", "third_party").put("rights", uint64_t{ubus_publish | ubus_resolve | ubus_list})
        .put("quota", uint64_t{8});
    request.data.cap("lifetime", *request.attach(lease.value, RIGHT_INSPECT | RIGHT_DUPLICATE));
    expect(root_directory.invoke("View", std::move(request), response));
    auto view_connector = response.take(response.data["connector"]); check(bool(view_connector));
    ubus_conn publisher; expect(publisher.bind(view_connector.selector()));
    check(!publisher.token("View") && !publisher.token("Role"));
    request = {}; request.data.put("name", "calculator").put("interface", "example.calculator").put("version", uint64_t{3});
    request.data.cap("connector", *request.attach(connector.selector(), RIGHT_SEND | RIGHT_DUPLICATE | RIGHT_REVOKE));
    expect(publisher.invoke("Publish", std::move(request), response));
    const auto published = response.data["instance"].as<uint64_t>(); check(published.has_value());
    request = {}; request.data.put("name", "public/calculator").put("instance", *published);
    expect(root_directory.invoke("Role", std::move(request), response));
    request = {}; request.data.put("prefix", "public").put("rights", uint64_t{ubus_resolve | ubus_list}).put("quota", uint64_t{8});
    request.data.cap("lifetime", *request.attach(env.scope, RIGHT_INSPECT | RIGHT_DUPLICATE));
    expect(root_directory.invoke("View", std::move(request), response));
    auto reader_connector = response.take(response.data["connector"]);
    ubus_conn reader; expect(reader.bind(reader_connector.selector()));
    request = {}; request.data.put("limit", uint64_t{8});
    expect(reader.invoke("List", std::move(request), response));
    check(response.data["items"][size_t{0}]["name"].as<std::string_view>() == "calculator");
    {
        // Independently linked generic client knows no calculator SDK.
        ch_pair output; expect(output.open(env.alloc(), 4, 0));
        const proc_cap inputs[]{
            input(ubus_directory, reader_connector.selector(), RIGHT_SEND | RIGHT_DUPLICATE),
            input(rt_stdout, output.ends[0].selector(), RIGHT_SEND, 1)};
        const std::array<std::string_view, 6> args{"bus", "call", "calculator", "sum", "a=11", "b=31"};
        const auto client = start(tasks, image, "client", {.argv=args, .caps=inputs,
            .resources={.bytes=2 * 1024 * 1024, .caps=128, .kinds=proc_base_kinds | OBJ_BIT(ObjChannel)},
            .events=env.events});
        ubus_rx stream; expect(stream.open(output.ends[1].selector(), env.events));
        str_chunk text;
        for (;;) {
            const auto status = str_receive(stream, text);
            if (status == STATUS_OK) break;
            check(status == STATUS_WOULD_BLOCK); expect(ev_wait(env.events).status);
        }
        check(std::string_view{text.data, text.size}.find("\"value\":42") != std::string_view::npos);
        for (;;) {
            const auto exited = tasks.collect(client);
            if (exited.status == STATUS_OK) { expect(static_cast<status_t>(exited.value)); break; }
            check(exited.status == STATUS_WOULD_BLOCK || proc_retryable(exited.status)); expect(ev_wait(env.events).status);
        }
    }
    ubus_conn through_role; expect(through_role.connect(reader, "calculator"));
    ubus_conn discovered; expect(discovered.connect(root_directory, "third_party/calculator"));
    request = {}; request.data.put("a", uint64_t{11}).put("b", uint64_t{31});
    expect(discovered.invoke("sum", std::move(request), response));
    check(response.data["value"].as<uint64_t>() == 42);
    request = {}; request.data.put("limit", uint64_t{8});
    expect(root_directory.invoke("List", std::move(request), response));
    const auto epoch = response.data["epoch"].as<uint64_t>(); check(epoch.has_value());
    request = {}; request.deadline = *rt_after_ms(1000); request.data.put("epoch", *epoch);
    expect(root_directory.begin("Changed", std::move(request)));
    expect(sys::scope_close(publish_scope.selector()).status);
    status_t changed_status;
    while ((changed_status = root_directory.poll(response)) == STATUS_WOULD_BLOCK) expect(ev_wait(env.events).status);
    expect(changed_status); expect(response.status);
    request = {}; request.data.put("name", "third_party/calculator");
    check(root_directory.invoke("Resolve", std::move(request), response) == STATUS_NOT_FOUND);
    request = {}; request.data.put("a", uint64_t{21}).put("b", uint64_t{21});
    expect(discovered.invoke("sum", std::move(request), response));
    check(response.data["value"].as<uint64_t>() == 42);
    expect(other.invoke("stop", {}, response));
    expect(wait(tasks, child));
    check(stop(tasks, registry) == STATUS_CANCELED);
    output.write("[ubus] dynamic methods, facets, capabilities, bulk and deferred views ok\n");
    output.write("[ubus] native I/O completion, deadline loan and close drain ok\n");
    output.write("[ubus] independent registry, views, discovery and publication lifetime ok\n");
    sys::exit();
}
