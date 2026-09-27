#include <user/lib/file_client.hpp>
#include <user/lib/stream.hpp>

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    using namespace myos;
    const auto info = service::bootstrap(address, size);
    if (info.argument_count() != 1
        && (info.argument_count() != 2 || !service::equal(info.argument(1), "/boot")))
        exit(MYOS_STATUS_BAD_ARGS);
    stream::Writer output{service::capability(info, bootstrap::imports::Stdout)};
    files::Client boot;
    auto status = boot.connect(info);
    if (status != MYOS_STATUS_OK) exit(status);
    io::ControlMessage entry{};
    do {
        status = boot.list(entry);
        if (status != MYOS_STATUS_OK) break;
        output.write(entry.data, entry.size);
    } while (entry.value != 0);
    const auto closed = boot.close();
    if (status == MYOS_STATUS_OK) status = closed;
    exit(status);
}
