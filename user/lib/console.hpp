#pragma once

#include <user/lib/service.hpp>

namespace myos::console {

// The UART queue alone can place a prompt after output from every writer.
enum class Operation : uint64_t { Bytes, Prompt };

inline void prompt(myos_cap_t output, const char* text) noexcept {
    service::Message message{.operation = static_cast<uint64_t>(Operation::Prompt)};
    message.size = service::length(text);
    if (message.size > sizeof(message.data)) exit(MYOS_STATUS_BAD_ARGS);
    service::copy(message.data, text, message.size);
    service::require(service::send(output, message).status);
}

} // namespace myos::console
