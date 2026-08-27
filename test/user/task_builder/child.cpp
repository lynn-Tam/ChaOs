#include <stdint.h>

#include <user/lib/syscall.hpp>

namespace {

/*Confirmatory experiment.
 * Exit condition: remove this fixture once the real process-server caller
 * supplies a stable PT_LOAD/BSS materialization consumer. */
volatile uint64_t initialized_data = UINT64_C(0x5354414745453030);
volatile uint8_t bss_tail[8192];

} // namespace

extern "C" [[noreturn]] void myos_main(
    const void* bootstrap,
    myos_word_t bootstrap_size) noexcept {
    static_cast<void>(bootstrap);
    static_cast<void>(bootstrap_size);
    bss_tail[0] = static_cast<uint8_t>(initialized_data);
    myos::exit();
}
