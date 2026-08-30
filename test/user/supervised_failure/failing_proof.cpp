#include <stddef.h>
#include <stdint.h>

#include <user/lib/bootstrap.hpp>
#include <user/lib/syscall.hpp>
#include <uapi/bootstrap.h>
#include <uapi/status.h>

/* Test-owned ordinary workload for the supervised-failure envelope.  It
 * follows the same bootstrap contract as programs/proof and publishes an
 * explicit non-OK execution result through the canonical exit ABI; no
 * production server or lifecycle bypass is involved. */
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
        myos::exit(MYOS_STATUS_BAD_ARGS);
    }

    /* The child has completed the same bootstrap validation as the
     * production proof, then publishes one non-OK terminal.  The unchanged
     * process_server and init supervisors must propagate that result through
     * their normal observe/close/refund paths. */
    myos::exit(MYOS_STATUS_INTERNAL);
}
