#include <unistd.h>
#include <sys/status.h>
#include <libk/parse.hpp>

int main(int argc, char** argv) {
    if (argc != 2) return invalid;
    const auto parsed = libk::parse<uint64_t>(argv[1]);
    if (!parsed) return invalid;
    auto count = *parsed;
    char bytes[96];
    for (size_t i = 0; i < sizeof(bytes); ++i) bytes[i] = 'a' + i % 26;
    while (count) {
        const auto size = count < sizeof(bytes) ? count : sizeof(bytes);
        const auto sent = write(STDOUT_FILENO, bytes, size);
        if (sent < 0) return sent;
        if (sent == 0) return io_error;
        count -= sent;
    }
    return 0;
}
