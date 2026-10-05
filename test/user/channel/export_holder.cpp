#include <test/user/channel/export_protocol.hpp>
#include <servers/runtime/service.hpp>

extern "C" [[noreturn]] void myos_main(const void* address, myos_word_t size) noexcept {
    using namespace myos;
    const auto info = service::bootstrap(address, size);
    const auto handoff = service::capability(info, bootstrap::imports::Stdout);
    const auto provider = service::capability(info, channel_test::Provider);
    service::require(service::send_cap(handoff, {.id = 1}, provider, MYOS_RIGHT_SEND).status);
    // Keep the import lineage live while the independent coordinator holds
    // its transferred copy. The coordinator stops this task after the provider
    // generation has retired.
    const auto parked = notification_create(
        service::capability(info, MYOS_BOOTSTRAP_CAP_RESOURCE_POOL), 1);
    service::require(parked.status);
    (void)notification_wait(parked.value);
    exit(MYOS_STATUS_INTERNAL);
}
