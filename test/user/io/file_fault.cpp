#include <user/lib/file_client.hpp>
#include "file_fault.hpp"

namespace { myos::files::Client filesystem; }
extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    using namespace myos;
    const auto info = service::bootstrap(address, size);
    service::require(filesystem.connect(info));
    files::File file;
    service::require(filesystem.open("DATA.BIN", 8, file));
    auto backing = filesystem.backing(file);
    if (!backing) exit(backing.error());
    auto mapping = MappedMemory::map(service::capability(info, MYOS_BOOTSTRAP_CAP_VSPACE),
        libk::move(backing.value().memory), 0x75000000, 4096, MYOS_VM_READ);
    if (!mapping) exit(mapping.error());
    service::require(filesystem.close(file));
    service::require(filesystem.close());
    service::require(notification_signal(service::capability(info, file_fault_test::Ready)).status);
    service::require(notification_wait(service::capability(info, file_fault_test::Go)).status);
    const auto* bytes = reinterpret_cast<const volatile uint8_t*>(mapping.value().address);
    for (size_t i = 0; i != 4096; ++i)
        if (bytes[i] != i % 251) exit(MYOS_STATUS_INTERNAL);
    service::require(mapping.value().close());
    exit();
}
