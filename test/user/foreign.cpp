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
    status = publisher.bind(rt_startup().selector(ubus_publisher));
    if (status != STATUS_OK) return status;
    const auto published = object.publish(publisher, "math");
    if (!published) return published.error();
    for (;;) {
        status = object.poll();
        if (status == STATUS_WOULD_BLOCK) status = ev_wait(rt_startup().events).status;
        if (status != STATUS_OK) return status;
    }
}
