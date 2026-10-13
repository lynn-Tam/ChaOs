#include <util.hpp>
#include <protocols/stream.hpp>
#include <test/user/channel/export_protocol.hpp>
#include <ipc.hpp>

int main(int argc, char** argv) {

    const auto& info = rt_startup();
    const auto control = info.selector(rt_stdout);
    if (argc == 2 && std::string_view{argv[1]} == "abandon") {
        ch_msg request;
        cap_owner reply;
        size_t count{};
        if (const auto rc = ch_recv(control, request, true, {&reply, 1}, &count).status;
            rc != STATUS_OK)
            sys::exit(rc);
        if (count != 1 || request.id != 73)
            sys::exit(STATUS_BAD_ARGS);
        if (const auto rc = reply.close(); rc != STATUS_OK)
            sys::exit(rc);
        return STATUS_OK;
    }
    const auto pair = sys::channel_create(info.pool, 4, CHANNEL_MAX_WORDS, 0, 1);
    if (pair.status != STATUS_OK)
        sys::exit(pair.status);
    const auto sender = sys::channel_mint(pair.value, info.cspace, 1, RIGHT_SEND | RIGHT_DUPLICATE);
    if (sender.status != STATUS_OK)
        sys::exit(sender.status);
    if (const auto rc =
            ch_send_cap(control, {.id = 1}, sender.value, RIGHT_SEND | RIGHT_DUPLICATE).status;
        rc != STATUS_OK)
        sys::exit(rc);
    const auto server = pair.value2;
    ch_msg request{};
    if (const auto rc = ch_recv(server, request).status; rc != STATUS_OK)
        sys::exit(rc);
    if (request.id != 41)
        sys::exit(STATUS_BAD_ARGS);
    if (const auto rc = ch_send(control, {.id = 4}).status; rc != STATUS_OK)
        sys::exit(rc);
    ch_msg command{};
    if (const auto rc = ch_recv(control, command).status; rc != STATUS_OK)
        sys::exit(rc);
    if (command.id != 3)
        sys::exit(STATUS_BAD_ARGS);
    // Provider death retires its channel, including copies held outside its group.
    sys::exit(STATUS_INTERNAL);
}
