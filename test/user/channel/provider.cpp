#include <util.hpp>
#include <protocols/stream.hpp>
#include <test/user/channel/export_protocol.hpp>
#include <protocols/msg.hpp>

int main(int argc, char** argv) {

    const auto& info = rt_startup();
    const auto control = info.selector(rt_stdout);
    if (argc == 2 && std::string_view{argv[1]} == "abandon") {
        msg_frame request;
        cap_owner reply;
        size_t count{};
        if (const auto rc = msg_recv(control, request, true, {&reply, 1}, &count).status;
            rc != STATUS_OK)
            sys::exit(rc);
        if (count != 1 || request.value != 73)
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
    const CapXfer transfer{sender.value, RIGHT_SEND | RIGHT_DUPLICATE, CAP_COPY, 0};
    if (const auto rc = msg_send(control, {.value = 1}, true, {&transfer, 1}).status; rc != STATUS_OK)
        sys::exit(rc);
    const auto server = pair.value2;
    msg_frame request{};
    if (const auto rc = msg_recv(server, request).status; rc != STATUS_OK)
        sys::exit(rc);
    if (request.value != 41)
        sys::exit(STATUS_BAD_ARGS);
    if (const auto rc = msg_send(control, {.value = 4}).status; rc != STATUS_OK)
        sys::exit(rc);
    msg_frame command{};
    if (const auto rc = msg_recv(control, command).status; rc != STATUS_OK)
        sys::exit(rc);
    if (command.value != 3)
        sys::exit(STATUS_BAD_ARGS);
    // Provider death retires its channel, including copies held outside its group.
    sys::exit(STATUS_INTERNAL);
}
