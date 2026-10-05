#pragma once

#include <servers/runtime/service.hpp>

namespace myos::stream {
// The channel supplies bounded backpressure. No reply queue is shared by
// writers; console access grants only the ability to enqueue output.
class Writer final {
    myos_cap_t output_;
public:
    explicit Writer(myos_cap_t output) noexcept : output_(output) {}
    void write(const char* text, size_t size) const noexcept {
        while (size != 0) {
            service::Message message{};
            message.size = size < sizeof(message.data) ? size : sizeof(message.data);
            service::copy(message.data, text, message.size);
            service::require(service::send(output_, message).status);
            text += message.size;
            size -= message.size;
        }
    }
    void write(const char* text) const noexcept { write(text, service::length(text)); }
    void put(char value) const noexcept { write(&value, 1); }
};


} // namespace myos::stream

