#include <util.hpp>
#include <protocols/stream.hpp>
#include <libk/parse.hpp>
#include <protocols/msg.hpp>
#include <test/user/channel/export_protocol.hpp>
#include <string_view>

int main(int argc, char** argv) {
    if (argc != 2) return STATUS_BAD_ARGS;

    const auto& info = rt_startup();
    if (std::string_view{argv[1]} == "await") {
        const auto ready = msg_send(info.selector(rt_stderr), {}).status;
        if (ready != STATUS_OK) sys::exit(ready);
        sys::exit(sys::wait_one(info.selector(ct_notice), 1).status);
    }
    if (std::string_view{argv[1]} == "compete") {
        const auto source = info.selector(rt_stdout), report = info.selector(rt_stderr);
        auto status = msg_send(report, {.value = UINT64_MAX}).status;
        if (status != STATUS_OK) return status;
        for (;;) {
            msg_frame message;
            status = msg_recv(source, message).status;
            if (status == STATUS_PEER_CLOSED) return STATUS_OK;
            if (status != STATUS_OK) return status;
            status = msg_send(report, message).status;
            if (status != STATUS_OK) return status;
        }
    }
    if (std::string_view{argv[1]} == "flood") {
        const cap_t endpoint = info.selector(rt_stdout), report = info.selector(rt_stderr);
        auto status = msg_send(report, {}).status;
        if (status != STATUS_OK) return status;
        status = sys::wait_one(info.selector(ct_notice), 1).status;
        if (status != STATUS_OK) return status;
        std::array<word_t, 256> latency;
        for (size_t round = 0; round < latency.size(); ++round) {
            const auto start = sys::clock_now().value;
            for (word_t i = 0; i < 4; ++i) {
                const word_t value = round * 4 + i;
                status = ch_send(endpoint, {&value, 1}).status;
                if (status != STATUS_OK) return status;
            }
            for (word_t i = 0; i < 4; ++i) {
                ch_packet packet;
                status = ch_recv(endpoint, packet).status;
                if (status != STATUS_OK) return status;
                if (packet.word_count != 1 || packet.count || packet.words[0] != round * 4 + i)
                    return STATUS_PEER_FAULT;
            }
            latency[round] = sys::clock_now().value - start;
        }
        std::sort(latency.begin(), latency.end());
        msg_frame result{.value = latency[latency.size() / 2], .size = sizeof(word_t)};
        const word_t p99 = latency[(latency.size() * 99 + 99) / 100 - 1];
        ::memcpy(result.data, &p99, sizeof(p99));
        return msg_send(report, result).status;
    }
    const auto id = libk::parse<uint64_t>(argv[1]);
    if (!id)
        sys::exit(STATUS_BAD_ARGS);
    const msg_frame message{.value = *id};
    if (const auto rc = msg_send(info.selector(rt_stderr), message).status; rc != STATUS_OK)
        sys::exit(rc);
    // Concurrent senders wait for actual credit before their messages commit.
    sys::exit(msg_send(info.selector(rt_stdout), message).status);
}
