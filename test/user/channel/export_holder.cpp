#include <sys/protocol.hpp>
#include <test/user/channel/export_protocol.hpp>
#include <sys/channel.hpp>

extern "C" [[noreturn]] void user_main(const void* address, word_t size) noexcept {
    using namespace sys;
    const auto info = sys::bootstrap(address, size);
    const auto handoff = sys::capability(info, boot::Stdout);
    const auto provider = sys::capability(info, channel_test::Provider);
    sys::require(sys::send_cap(handoff, {.id = 1}, provider, RIGHT_SEND).status);
    // Keep the import lineage live while the independent coordinator holds
    // its transferred copy. The coordinator stops this task after the provider
    // generation has retired.
    const auto parked = notification_create(
        sys::capability(info, BOOT_POOL), 1);
    sys::require(parked.status);
    (void)notification_wait(parked.value);
    exit(STATUS_INTERNAL);
}
