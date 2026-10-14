#include <util.hpp>
#include <protocols/stream.hpp>
#include <libk/parse.hpp>
#include <protocols/msg.hpp>

int main(int argc, char** argv) {
    if (argc != 2) return STATUS_BAD_ARGS;

    const auto& info = rt_startup();
    const auto id = libk::parse<uint64_t>(argv[1]);
    if (!id)
        sys::exit(STATUS_BAD_ARGS);
    const msg_frame message{.value = *id};
    if (const auto rc = msg_send(info.selector(rt_stderr), message).status; rc != STATUS_OK)
        sys::exit(rc);
    // No retry: multiple writers must be admitted as ordinary blocking sends.
    sys::exit(msg_send(info.selector(rt_stdout), message).status);
}
