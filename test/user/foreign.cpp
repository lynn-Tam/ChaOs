#include <ubus/ubus.hpp>

// An ordinary package: no OS service SDK, IDL, launcher table or private codec.
int main() {
    ubus_object object{"example.math"};
    auto status = object.add("Add", {{"a", ubus_type::U64}, {"b", ubus_type::U64}}, {{"sum", ubus_type::U64}},
        [](auto& request, auto& response) {
            const auto a = *request.data["a"].template as<uint64_t>(), b = *request.data["b"].template as<uint64_t>();
            if (a > UINT64_MAX - b) return STATUS_BAD_ARGS;
            response.data.put("sum", a + b); return STATUS_OK;
        });
    if (status == STATUS_OK) status = object.open();
    if (status != STATUS_OK) return status;
    ubus_conn publisher;
    status = publisher.bind(rt_startup().environment);
    if (status != STATUS_OK) return status;
    auto connector = object.connector(); if (!connector) return connector.error();
    ubus_msg request, response;
    const auto peer_idx = request.attach(connector->selector(), RIGHT_SEND | RIGHT_DUPLICATE);
    if (!peer_idx) return peer_idx.error();
    request.data.put("name", "math").put("interface", "example.math").put("version", uint64_t{1})
        .cap("connector", *peer_idx);
    status = publisher.invoke("Publish", std::move(request), response); if (status != STATUS_OK) return status;
    for (;;) {
        status = object.poll();
        if (status == STATUS_WOULD_BLOCK) status = ev_wait(rt_startup().events).status;
        if (status != STATUS_OK) return status;
    }
}
