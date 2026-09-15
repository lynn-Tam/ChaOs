#include <user/lib/stream.hpp>
#include <user/lib/service.hpp>

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    const auto info = myos::service::bootstrap(address, size);
    const myos::stream::Writer output{myos::service::capability(info, myos::bootstrap::imports::Stdout)};
    for (size_t i = 1; i < info.argument_count(); ++i) {
        if (i != 1) output.put(' ');
        output.write(info.argument(i));
    }
    output.put('\n');
    myos::exit();
}
