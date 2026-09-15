#include <user/lib/file_client.hpp>

namespace { myos::files::Client filesystem; }

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    using namespace myos;
    const auto info = service::bootstrap(address, size);
    service::require(filesystem.connect(info));
    files::File file;
    service::require(filesystem.open("DATA.BIN", 8, file));
    // The fixture injects EIO at the file's first data sector, after mounting.
    // Ordinary reads and page-in traverse the same real block service.
    const auto read = filesystem.read(file, [](uint64_t, const uint8_t*, size_t) {});
    if (read != MYOS_STATUS_BACKING_FAILED) exit(MYOS_STATUS_INTERNAL);
    auto backing = filesystem.backing(file);
    if (!backing) exit(backing.error());
    auto mapping = MappedMemory::map(service::capability(info, MYOS_BOOTSTRAP_CAP_VSPACE),
        libk::move(backing.value().memory), 0x75000000, (file.size + 4095) & ~size_t{4095}, MYOS_VM_READ);
    if (!mapping) exit(mapping.error());
    service::require(filesystem.close(file));
    service::require(filesystem.close());
    // Closing both handles must preserve the independent content authority.
    // Its first fault must report the backend failure, never fabricate zeros.
    const auto byte = *reinterpret_cast<const volatile uint8_t*>(mapping.value().address);
    (void)byte;
    exit(MYOS_STATUS_INTERNAL);
}
