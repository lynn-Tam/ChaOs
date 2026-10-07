#include <test/user/channel/export_protocol.hpp>
#include <servers/runtime/service.hpp>

extern "C" [[noreturn]] void user_main(const void* address, word_t size) noexcept {
    using namespace sys;
    const auto info = service::bootstrap(address, size);
    const auto control = service::capability(info, boot::Stdout);
    const auto server = service::capability(info, channel_test::Provider);
    service::Message request{};
    service::require(service::receive(server, request).status);
    if (request.id != 41) exit(STATUS_BAD_ARGS);
    service::require(service::send(control, {.id = 4}).status);
    service::Message command{};
    service::require(service::receive(control, command).status);
    if (command.id != 3) exit(STATUS_BAD_ARGS);
    // Ordinary non-OK termination retires the provider's prepared export.
    exit(STATUS_INTERNAL);
}
