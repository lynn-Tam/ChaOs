#pragma once

#include <servers/runtime/service.hpp>
#include <sys/channel.hpp>

#include <servers/runtime/output.hpp>

#include <servers/uart/protocol.hpp>

namespace sys::console {
inline void prompt(cap_t output, const char* text) noexcept {
    service::Message message{.operation = static_cast<uint64_t>(Operation::Prompt)};
    message.size = service::length(text);
    if (message.size > sizeof(message.data)) exit(STATUS_BAD_ARGS);
    service::copy(message.data, text, message.size);
    service::require(service::send(output, message).status);
}

}

namespace sys::terminal {

enum class LineResult { Line, TooLong, End };

class LineReader final {
    cap_t input_;
    stream::Writer output_;
public:
    LineReader(cap_t input, cap_t output) noexcept
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
                exit(STATUS_PEER_FAULT);
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

} // namespace sys::terminal
