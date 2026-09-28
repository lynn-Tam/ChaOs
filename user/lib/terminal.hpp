#pragma once

#include <user/lib/stream.hpp>

namespace myos::terminal {

enum class LineResult { Line, TooLong, End };

class ByteReader final {
    myos_cap_t input_;
    service::Message message_{};
    size_t next_{};
public:
    explicit ByteReader(myos_cap_t input) noexcept : input_(input) {}

    [[nodiscard]] auto read() noexcept -> int {
        while (next_ == message_.size) {
            service::require(service::receive(input_, message_).status);
            if (message_.operation == static_cast<uint64_t>(stream::Frame::End)) return -1;
            if (message_.operation != static_cast<uint64_t>(stream::Frame::Data)
                || message_.size == 0 || message_.size > sizeof(message_.data))
                exit(MYOS_STATUS_PEER_FAULT);
            next_ = 0;
        }
        return static_cast<uint8_t>(message_.data[next_++]);
    }
};

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
