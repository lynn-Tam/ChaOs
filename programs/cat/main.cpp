#include <user/lib/file_client.hpp>

namespace { myos::files::Client filesystem; }
extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    using namespace myos;
    const auto info = service::bootstrap(address, size);
    if (info.argument_count() != 2) exit(MYOS_STATUS_BAD_ARGS);
    service::require(filesystem.connect(info));
    files::File file;
    service::Console output{service::capability(info, bootstrap::imports::ConsoleOutput)};
    auto status = filesystem.open(info.argument(1), service::length(info.argument(1)), file);
    if (status == MYOS_STATUS_OK) {
        status = filesystem.read(file, [&](uint64_t, const uint8_t* data, size_t bytes) {
            output.write(reinterpret_cast<const char*>(data), bytes);
        });
        const auto closed = filesystem.close(file);
        if (status == MYOS_STATUS_OK) status = closed;
    }
    const auto disconnected = filesystem.close();
    if (status == MYOS_STATUS_OK) status = disconnected;
    exit(status);
}
