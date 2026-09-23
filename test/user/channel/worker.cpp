#include <user/lib/clock.hpp>
#include <user/lib/service.hpp>

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    using namespace myos;
    const auto info = service::bootstrap(address, size);
    const auto id = decimal(info.argument(1));
    if (!id) exit(MYOS_STATUS_BAD_ARGS);
    const service::Message message{.id = *id};
    service::require(service::send(service::capability(info, bootstrap::imports::Stderr), message).status);
    // No retry: multiple writers must be admitted as ordinary blocking sends.
    exit(service::send(service::capability(info, bootstrap::imports::Stdout), message).status);
}
