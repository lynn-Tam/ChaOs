#include <libk/parse.hpp>
#include <servers/runtime/service.hpp>

extern "C" [[noreturn]] void user_main(const void* address, word_t size, const char* arg_data, size_t arg_size) noexcept {
    using namespace sys;
    const auto info = service::bootstrap(address, size);
    boot::Args args;
    if (!args.decode(arg_data, arg_size)) exit(STATUS_BAD_ARGS);
    const auto id = libk::parse<uint64_t>(args.argument(1));
    if (!id) exit(STATUS_BAD_ARGS);
    const service::Message message{.id = *id};
    service::require(service::send(service::capability(info, boot::Stderr), message).status);
    // No retry: multiple writers must be admitted as ordinary blocking sends.
    exit(service::send(service::capability(info, boot::Stdout), message).status);
}
