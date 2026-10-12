#include <stddef.h>

#include <libk/assert.hpp>
#include <sys/syscall.hpp>


using Constructor = void (*)();


extern "C" Constructor __init_array_start[];
extern "C" Constructor __init_array_end[];

extern "C" void run_constructors() noexcept {
    for (Constructor* constructor = __init_array_start;
         constructor != __init_array_end;
         ++constructor) {
        (*constructor)();
    }
}

[[noreturn]] void libk::assert_fail(const libk::assert_info&) noexcept {
    sys::exit();
}


extern "C" [[noreturn]] void abort() noexcept { sys::exit(STATUS_INTERNAL); }
