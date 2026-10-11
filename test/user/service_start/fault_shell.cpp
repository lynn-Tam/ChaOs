#include <sys/channel.hpp>
#include <sys/protocol.hpp>

extern "C" [[noreturn]] void user_main(const void* address, word_t size) noexcept {
    using namespace sys;
    const auto info = sys::bootstrap(address, size);
    io::ClientSession files;
    uint64_t value{};
    sys::require(files.connect(sys::capability(info, boot::Files),
        sys::capability(info, BOOT_POOL), sys::capability(info, BOOT_CSPACE),
        sys::capability(info, BOOT_EVENTS), sys::capability(info, BOOT_VSPACE), 0x70000000, value));
    fs::File file{};
    constexpr char path[] = "README.TXT";
    sys::require(fs::open(files, path, sizeof(path) - 1, file));
    size_t read{};
    sys::require(fs::read(files, file, [&](uint64_t, const uint8_t*, size_t bytes) noexcept {
        read += bytes;
    }));
    if (read != file.size || read == 0) exit(STATUS_INTERNAL);
    sys::require(fs::close(files, file));
    sys::require(files.close());
    stream::Writer console{sys::capability(info, boot::ConsoleOutput)};
    console.write("[fault-shell] file read before failure\n");
    // This test ELF takes a real user fault. Production services contain no
    // fault trigger or scenario branch.
    (void)*reinterpret_cast<volatile const uint64_t*>(0xe100);
    exit(STATUS_INTERNAL);
}
