#include <user/lib/clock.hpp>
#include <user/lib/service.hpp>

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    using namespace myos;
    const auto info = service::bootstrap(address, size);
    const auto duration = info.argument_count() == 2 ? decimal(info.argument(1)) : libk::nullopt;
    if (!duration) exit(MYOS_STATUS_BAD_ARGS);
    Clock clock;
    service::require(clock.open());
    const auto deadline = clock.after_ms(*duration);
    if (!deadline) exit(MYOS_STATUS_BAD_ARGS);
    const auto notification = notification_create(service::capability(info, MYOS_BOOTSTRAP_CAP_RESOURCE_POOL), 1);
    service::require(notification.status);
    const auto status = notification_wait(notification.value, *deadline).status;
    service::require(object_destroy(notification.value).status);
    service::require(cap_close(notification.value).status);
    exit(status == MYOS_STATUS_TIMED_OUT ? MYOS_STATUS_OK : status);
}
