#include <user/lib/stream.hpp>
#include <user/lib/clock.hpp>

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    using namespace myos;
    const auto info = service::bootstrap(address, size);
    const auto count = info.argument_count() == 2 ? decimal(info.argument(1)) : libk::nullopt;
    if (!count) exit(MYOS_STATUS_BAD_ARGS);
    stream::Writer output{service::capability(info, bootstrap::imports::Stdout)};
    char bytes[96];
    for (size_t i = 0; i < sizeof(bytes); ++i) bytes[i] = 'a' + i % 26;
    uint64_t remaining = *count;
    while (remaining) {
        const auto size = remaining < sizeof(bytes) ? remaining : sizeof(bytes);
        output.write(bytes, size);
        remaining -= size;
    }
    exit();
}
