#include <user/lib/bootstrap.hpp>
#include <user/lib/syscall.hpp>
#include <uapi/bootstrap.h>
#include <uapi/status.h>

namespace {

constexpr myos_word_t DataAddress = 0x4200'0000;
constexpr myos_word_t Value = 0x5a17'c0de;

[[noreturn]] void finish(myos_status_t status) noexcept {
    myos::exit(status);
}

[[noreturn]] void run(const myos::bootstrap::BootstrapView& info) noexcept {
    if (info.selector(MYOS_BOOTSTRAP_CAP_VSPACE) == 0
        || info.selector(MYOS_BOOTSTRAP_CAP_CSPACE) == 0) {
        finish(MYOS_STATUS_BAD_ARGS);
    }

    auto* const page = reinterpret_cast<volatile myos_word_t*>(DataAddress);
    if (*page != 0) {
        finish(MYOS_STATUS_INTERNAL);
    }
    *page = Value;
    if (*page != Value) {
        finish(MYOS_STATUS_INTERNAL);
    }
    finish(MYOS_STATUS_OK);
}

} // namespace

extern "C" [[noreturn]] void myos_main(
    const void* bootstrap,
    myos_word_t bootstrap_size) noexcept {
    const auto info = myos::bootstrap::BootstrapView::parse(
        bootstrap, bootstrap_size);
    if (!info || info->cpu_count() == 0) {
        finish(MYOS_STATUS_BAD_ARGS);
    }
    run(*info);
}
