#include <user/lib/service.hpp>

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    const auto info = myos::service::bootstrap(address, size);
    myos::service::Console{myos::service::capability(info, MYOS_BOOTSTRAP_CAP_CONSOLE_OUTPUT)}
        .write("Hello from userspace.\n");
    myos::exit();
}
