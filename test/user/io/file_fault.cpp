#include <utility>
#include <sys/channel.hpp>
#include <sys/protocol.hpp>
#include "file_fault.hpp"

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
    auto backing = fs::backing(filesystem, file);
    if (!backing) exit(backing.error());
    auto mapping = mapping::map(sys::capability(info, BOOT_VSPACE),
        std::move(backing->memory), 0x75000000, 4096, VM_READ);
    if (!mapping) exit(mapping.error());
    sys::require(fs::close(filesystem, file));
    sys::require(filesystem.close());
    sys::require(notification_signal(sys::capability(info, file_fault_test::Ready)).status);
    sys::require(notification_wait(sys::capability(info, file_fault_test::Go)).status);
    const auto* bytes = reinterpret_cast<const volatile uint8_t*>(mapping->address);
    for (size_t i = 0; i != 4096; ++i)
        if (bytes[i] != i % 251) exit(STATUS_INTERNAL);
    sys::require(mapping->close());
    exit();
}
