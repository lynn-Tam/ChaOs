#include <util.hpp>
#include <protocols/stream.hpp>
#include <test/user/channel/export_protocol.hpp>
#include <protocols/msg.hpp>

int main() {

    const auto& info = rt_startup();
    const auto handoff = info.selector(rt_stdout);
    const auto provider = info.selector(ct_provider);
    const CapXfer transfer{provider, RIGHT_SEND, CAP_COPY, 0};
    if (const auto rc = msg_send(handoff, {.value = 1}, true, {&transfer, 1}).status; rc != STATUS_OK)
        sys::exit(rc);
    // Keep the import lineage live while the independent coordinator holds
    // its transferred copy. The coordinator stops this task after the provider
    // generation has retired.
    (void)sys::wait_one(info.events, 0);
    sys::exit(STATUS_INTERNAL);
}
