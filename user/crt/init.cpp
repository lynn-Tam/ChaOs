#include <stddef.h>

#include <libk/assert.hpp>
#include <user/abi/calls.hpp>

namespace {
using Constructor = void (*)();
}

extern "C" Constructor __init_array_start[];
extern "C" Constructor __init_array_end[];

extern "C" void run_constructors() noexcept {
    for (Constructor* constructor = __init_array_start;
         constructor != __init_array_end;
         ++constructor) {
        (*constructor)();
    }
}

namespace libk {
[[noreturn]] void assert_fail(const AssertInfo&) noexcept {
    myos::exit();
}
} // namespace libk
