#include <sys/protocol.hpp>
#include <test/user/channel/export_protocol.hpp>
#include <sys/channel.hpp>

extern "C" [[noreturn]] void user_main(const void* address, word_t size) noexcept {
    using namespace sys;
    const auto info = sys::bootstrap(address, size);
    const auto control = sys::capability(info, boot::Stdout);
    const auto pair = channel_create(info.selector(BOOT_POOL), 4, CHANNEL_MAX_WORDS, 0, 1);
    sys::require(pair.status);
    const auto sender = channel_mint(pair.value, info.selector(BOOT_CSPACE), 1, RIGHT_SEND | RIGHT_DUPLICATE);
    sys::require(sender.status);
    sys::require(sys::send_cap(control, {.id = 1}, sender.value, RIGHT_SEND | RIGHT_DUPLICATE).status);
    const auto server = pair.value2;
    sys::Message request{};
    sys::require(sys::receive(server, request).status);
    if (request.id != 41) exit(STATUS_BAD_ARGS);
    sys::require(sys::send(control, {.id = 4}).status);
    sys::Message command{};
    sys::require(sys::receive(control, command).status);
    if (command.id != 3) exit(STATUS_BAD_ARGS);
    // Provider death retires its channel, including copies held outside its group.
    exit(STATUS_INTERNAL);
}
