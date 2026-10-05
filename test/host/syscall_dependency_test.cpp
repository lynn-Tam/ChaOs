#include <stddef.h>

#include <sys/syscall.hpp>

#if defined(DEPLOY_MAGIC) || defined(MYOS_BOOT_MAGIC)
#error "raw syscall.hpp must not depend on deployment or BootBundle"
#endif

static_assert(sizeof(myos::SysResult) == 3 * sizeof(myos_word_t));

int main() {
    return 0;
}
