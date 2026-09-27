#include <user/lib/vfs_client.hpp>
#include <user/lib/stream.hpp>

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    using namespace myos;
    const auto info = service::bootstrap(address, size);
    const bool append = info.argument_count() == 3
        && service::equal(info.argument(1), "-a");
    if (!append && info.argument_count() != 2) exit(MYOS_STATUS_BAD_ARGS);
    vfs::Client storage;
    service::require(storage.connect(info));
    vfs::File file{};
    auto status = storage.open(info.argument(append ? 2 : 1),
        vfs::Write | vfs::Create | (append ? uint64_t{0} : vfs::Truncate), file);
    if (status != MYOS_STATUS_OK) exit(status);
    stream::Reader input;
    status = input.open(service::capability(info, bootstrap::imports::Stdin),
        service::capability(info, MYOS_BOOTSTRAP_CAP_SERVICE_NOTIFICATION));
    uint64_t offset = append ? file.size : 0;
    while (status == MYOS_STATUS_OK) {
        service::Message message;
        status = input.read(message);
        if (status != MYOS_STATUS_OK || message.size == 0) break;
        status = storage.write(file, offset,
            reinterpret_cast<const uint8_t*>(message.data), message.size);
        offset += message.size;
    }
    if (status == MYOS_STATUS_OK) status = storage.sync(file);
    const auto closed = storage.close(file);
    if (status == MYOS_STATUS_OK) status = closed;
    const auto disconnected = storage.close();
    if (status == MYOS_STATUS_OK) status = disconnected;
    exit(status);
}
