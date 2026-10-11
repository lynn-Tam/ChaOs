#include <utility>
#include <sys/channel.hpp>
#include <sys/protocol.hpp>

namespace { io::ClientSession filesystem; }

extern "C" [[noreturn]] void user_main(const void* address, word_t size) noexcept {
    using namespace sys;
    const auto info = sys::bootstrap(address, size);
    uint64_t value{};
    sys::require(filesystem.connect(sys::capability(info, boot::Files),
        sys::capability(info, BOOT_POOL), sys::capability(info, BOOT_CSPACE),
        sys::capability(info, BOOT_EVENTS), sys::capability(info, BOOT_VSPACE), 0x70000000, value));
    fs::File file;
    sys::require(fs::open(filesystem, "DATA.BIN", 8, file));
    // The fixture injects EIO at the file's first data sector, after mounting.
    // Ordinary reads and page-in traverse the same real block service.
    const auto read = fs::read(filesystem, file, [](uint64_t, const uint8_t*, size_t) {});
    if (read != STATUS_BACKING_FAILED) exit(STATUS_INTERNAL);
    auto backing = fs::backing(filesystem, file);
    if (!backing) exit(backing.error());
    auto mapping = mapping::map(sys::capability(info, BOOT_VSPACE),
        std::move(backing->memory), 0x75000000, (file.size + 4095) & ~size_t{4095}, VM_READ);
    if (!mapping) exit(mapping.error());
    sys::require(fs::close(filesystem, file));
    sys::require(filesystem.close());
    // Closing both handles must preserve the independent content authority.
    // Its first fault must report the backend failure, never fabricate zeros.
    const auto byte = *reinterpret_cast<const volatile uint8_t*>(mapping->address);
    (void)byte;
    exit(STATUS_INTERNAL);
}
