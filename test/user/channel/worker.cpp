#include <sys/protocol.hpp>
#include <libk/parse.hpp>
#include <sys/channel.hpp>

extern "C" [[noreturn]] void user_main(const void* address, word_t size, const char* arg_data, size_t arg_size) noexcept {
    using namespace sys;
    const auto info = sys::bootstrap(address, size);
    boot::args args;
    if (!args.decode(arg_data, arg_size)) exit(STATUS_BAD_ARGS);
    const auto id = libk::parse<uint64_t>(args.argument(1));
    if (!id) exit(STATUS_BAD_ARGS);
    const sys::Message message{.id = *id};
    sys::require(sys::send(sys::capability(info, boot::Stderr), message).status);
    // No retry: multiple writers must be admitted as ordinary blocking sends.
    exit(sys::send(sys::capability(info, boot::Stdout), message).status);
}
