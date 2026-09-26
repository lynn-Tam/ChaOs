#include <user/lib/store_client.hpp>
#include <user/lib/stream.hpp>

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    using namespace myos;
    const auto info = service::bootstrap(address, size);
    if (info.argument_count() != 2) exit(MYOS_STATUS_BAD_ARGS);
    store::Client storage;
    service::require(storage.connect(info, bootstrap::imports::StoreRead));
    store::File file{};
    auto status = storage.open(info.argument(1), store::Read, file);
    if (status == MYOS_STATUS_OK) {
        stream::Writer output{service::capability(info, bootstrap::imports::Stdout)};
        status = storage.read(file, [&](uint64_t, const uint8_t* data, size_t bytes) {
            output.write(reinterpret_cast<const char*>(data), bytes);
        });
        const auto closed = storage.close(file);
        if (status == MYOS_STATUS_OK) status = closed;
    }
    const auto disconnected = storage.close();
    if (status == MYOS_STATUS_OK) status = disconnected;
    exit(status);
}
