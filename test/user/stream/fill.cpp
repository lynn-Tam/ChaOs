#include <posix.hpp>
#include <start.hpp>
#include <lib/stream/stream.hpp>
#include <unistd.h>
#include <cerrno>
#include <uapi/abi.h>
#include <libk/parse.hpp>

int main(int argc, char** argv) {
    if (const auto rc = px_init(); rc != STATUS_OK) return rc;

    if (argc != 2) return STATUS_BAD_ARGS;
    const auto parsed = libk::parse<uint64_t>(argv[1]);
    if (!parsed) return STATUS_BAD_ARGS;
    auto count = *parsed;
    char bytes[96];
    for (size_t i = 0; i < sizeof(bytes); ++i) bytes[i] = 'a' + i % 26;
    while (count) {
        const auto size = count < sizeof(bytes) ? count : sizeof(bytes);
        const auto sent = write(STDOUT_FILENO, bytes, size);
        if (sent < 0) return errno;
        if (sent == 0) return STATUS_BACKING_FAILED;
        count -= sent;
    }
    return 0;
}
