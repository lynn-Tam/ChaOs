#include <util.hpp>
#include <protocols/stream.hpp>
#include <libk/parse.hpp>
#include <ipc.hpp>

int main(int argc, char** argv) {
    if (argc != 2) return STATUS_BAD_ARGS;

    const auto& info = rt_startup();
    const auto id = libk::parse<uint64_t>(argv[1]);
    if (!id)
        sys::exit(STATUS_BAD_ARGS);
    const ch_msg message{.id = *id};
    if (const auto rc = ch_send(info.selector(rt_stderr), message).status; rc != STATUS_OK)
        sys::exit(rc);
    // No retry: multiple writers must be admitted as ordinary blocking sends.
    sys::exit(ch_send(info.selector(rt_stdout), message).status);
}
