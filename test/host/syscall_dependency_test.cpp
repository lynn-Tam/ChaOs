#include <stddef.h>

#include <sys/syscall.hpp>

#if defined(DEPLOY_MAGIC) || defined(BUNDLE_MAGIC)
#error "raw syscall.hpp must not depend on deployment or BootBundle"
#endif

static_assert(sizeof(sys::SysResult) == 3 * sizeof(word_t));

int main() {
    return 0;
}
