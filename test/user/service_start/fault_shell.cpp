#include <user/lib/file_client.hpp>
#include <user/lib/stream.hpp>

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    using namespace myos;
    const auto info = service::bootstrap(address, size);
    files::Client files;
    service::require(files.connect(info));
    files::File file{};
    constexpr char path[] = "README.TXT";
    service::require(files.open(path, sizeof(path) - 1, file));
    size_t read{};
    service::require(files.read(file, [&](uint64_t, const uint8_t*, size_t bytes) noexcept {
        read += bytes;
    }));
    if (read != file.size || read == 0) exit(MYOS_STATUS_INTERNAL);
    service::require(files.close(file));
    service::require(files.close());
    stream::Writer console{service::capability(info, bootstrap::imports::ConsoleOutput)};
    console.write("[fault-shell] file read before failure\n");
    // This test ELF takes a real user fault. Production services contain no
    // fault trigger or scenario branch.
    (void)*reinterpret_cast<volatile const uint64_t*>(0xe100);
    exit(MYOS_STATUS_INTERNAL);
}
