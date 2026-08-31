#include <stddef.h>
#include <stdint.h>

#include <user/lib/bootstrap.hpp>
#include <user/lib/syscall.hpp>
#include <uapi/bootstrap.h>

namespace {

[[noreturn]] void fail(myos_status_t status) noexcept {
    myos::exit(status);
}

} // namespace

extern "C" [[noreturn]] void myos_main(
    myos_word_t bootstrap_address,
    myos_word_t bootstrap_size) noexcept {
    const auto bootstrap = myos::bootstrap::BootstrapView::parse(
        reinterpret_cast<const void*>(bootstrap_address), bootstrap_size);
    if (!bootstrap || bootstrap->cpu_count() == 0
        || bootstrap->bundle_size() == 0
        || bootstrap->selector(MYOS_BOOTSTRAP_CAP_RESOURCE_POOL) == 0
        || bootstrap->selector(MYOS_BOOTSTRAP_CAP_VSPACE) == 0
        || bootstrap->selector(MYOS_BOOTSTRAP_CAP_CSPACE) == 0
        || bootstrap->selector(MYOS_BOOTSTRAP_CAP_SCHED_DOMAIN) == 0
        || bootstrap->selector(MYOS_BOOTSTRAP_CAP_BOOT_BUNDLE) == 0) {
        fail(MYOS_STATUS_BAD_ARGS);
    }

    /* The ordinary workload has no console or supervisor protocol.  Its
     * completion is the canonical MYOS_SYS_EXIT terminal observed by
     * process_server. */
    myos::exit();
}
