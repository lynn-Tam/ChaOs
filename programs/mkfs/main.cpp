#include <user/lib/store_client.hpp>

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    using namespace myos;
    const auto info = service::bootstrap(address, size);
    if (info.argument_count() != 1 && info.argument_count() != 2) exit(MYOS_STATUS_BAD_ARGS);
    uint8_t id[store::VolumeIdSize]{};
    const bool identified = info.argument_count() == 2;
    if (identified && !store::parse_volume_id(info.argument(1),
        service::length(info.argument(1)), id)) exit(MYOS_STATUS_BAD_ARGS);
    store::Client storage;
    auto status = storage.connect(info, bootstrap::imports::StoreAdmin);
    if (status == MYOS_STATUS_OK) status = storage.format(identified ? id : nullptr);
    const auto closed = storage.close();
    if (status == MYOS_STATUS_OK) status = closed;
    exit(status);
}
