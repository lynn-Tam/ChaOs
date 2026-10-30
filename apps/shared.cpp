#include <ubus/ubus.hpp>

int main() {
    ubus_conn parent;
    auto status = parent.bind(_startup().bootstrap); if (status != STATUS_OK) return status;
    ubus_msg setup;
    status = parent.invoke("Receive", {}, setup); if (status != STATUS_OK) return status;
    const auto memory = setup.data["message"]["bar2"].as<ubus_buffer>();
    const auto backing = setup.capability(setup.data["message"]["bar2"]);
    if (!memory || !backing || memory->offset || !memory->size) return STATUS_BAD_ARGS;
    ubus_object object{"io.shared"};
    status = object.add("Get", {}, {{"memory", ubus_type::Buffer}}, [&](auto&, auto& reply) -> status_t {
        const auto index = reply.attach(backing, RIGHT_MAP | RIGHT_DUPLICATE); if (!index) return index.error();
        reply.data.put("memory", ubus_buffer{*index, 0, memory->size}); return STATUS_OK;
    });
    if (status == STATUS_OK) status = object.open();
    if (status != STATUS_OK) return status;
    auto connector = object.connector(); if (!connector) return connector.error();
    ubus_msg announce, reply;
    const auto index = announce.attach(connector->selector(), RIGHT_SEND | RIGHT_DUPLICATE); if (!index) return index.error();
    announce.data.put("name", "shared").put("interface", "io.shared").put("version", uint64_t{1}).cap("connector", *index);
    status = parent.invoke("Publish", std::move(announce), reply); if (status != STATUS_OK) return status;
    for (;;) {
        status = object.poll();
        if (status == STATUS_WOULD_BLOCK) status = _ev_wait(&_events()).status;
        if (status != STATUS_OK) return status;
    }
}
