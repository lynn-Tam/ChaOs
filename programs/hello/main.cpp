#include <user/lib/imports.hpp>
#include <user/lib/service.hpp>

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    const auto info = myos::service::bootstrap(address, size);
    myos::service::Console{myos::service::capability(info, myos::bootstrap::imports::ConsoleOutput)}
        .write("Hello from userspace.\n");
    myos::exit();
}
