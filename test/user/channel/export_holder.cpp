#include <util.hpp>
#include <protocols/stream.hpp>
#include <test/user/channel/export_protocol.hpp>
#include <ipc.hpp>

int main() {

    const auto& info = rt_startup();
    const auto handoff = info.selector(rt_stdout);
    const auto provider = info.selector(ct_provider);
    if (const auto rc = ch_send_cap(handoff, {.id = 1}, provider, RIGHT_SEND).status;
        rc != STATUS_OK)
        sys::exit(rc);
    // Keep the import lineage live while the independent coordinator holds
    // its transferred copy. The coordinator stops this task after the provider
    // generation has retired.
    const auto parked = sys::notification_create(info.pool, 1);
    if (parked.status != STATUS_OK) sys::exit(parked.status);
    (void) sys::notification_wait(parked.value);
    sys::exit(STATUS_INTERNAL);
}
