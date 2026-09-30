#pragma once

#include <user/server_rt/service.hpp>
#include <user/ipc/channel.hpp>

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

namespace myos::terminal {

enum class LineResult { Line, TooLong, End };

class LineReader final {
    myos_cap_t input_;
    stream::Writer output_;
public:
    LineReader(myos_cap_t input, myos_cap_t output) noexcept
        : input_(input), output_(output) {}

    auto read(char (&line)[128]) noexcept -> LineResult {
        size_t used{};
        bool overflow{};
        for (;;) {
            service::Message message{};
            service::require(service::receive(input_, message).status);
            if (message.operation == static_cast<uint64_t>(stream::Frame::End))
                return LineResult::End;
            if (message.operation != static_cast<uint64_t>(stream::Frame::Data))
                exit(MYOS_STATUS_PEER_FAULT);
            for (size_t i = 0; i < message.size; ++i) {
                const char byte = message.data[i];
                if (byte == '\n') {
                    output_.put('\n');
                    line[used] = '\0';
                    return overflow ? LineResult::TooLong : LineResult::Line;
                }
                if (byte == '\b' || byte == 127) {
                    if (used != 0) { --used; output_.write("\b \b"); }
                } else if (byte >= 32 && byte < 127) {
                    if (used + 1 < sizeof(line)) { line[used++] = byte; output_.put(byte); }
                    else overflow = true;
                }
            }
        }
    }
};

} // namespace myos::terminal
