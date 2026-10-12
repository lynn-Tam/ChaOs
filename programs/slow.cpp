#include <unistd.h>
#include <time.h>
#include <sys/status.h>
#include <libk/parse.hpp>
#include <libk/fmt.hpp>

namespace {
struct output {
    void write(const char* data, size_t size) { ::write(STDOUT_FILENO, data, size); }
};
}

// A paced byte counter; optional LIMIT exits before EOF.
int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) return invalid;
    uint64_t args[]{0, UINT64_MAX};
    for (int i = 1; i < argc; ++i) {
        const auto parsed = libk::parse<uint64_t>(argv[i]);
        if (!parsed) return invalid;
        args[i - 1] = *parsed;
    }
    const timespec delay{static_cast<int64_t>(args[0] / 1000),
        static_cast<int64_t>(args[0] % 1000 * 1'000'000)};
    uint64_t count{};
    char bytes[96];
    while (count < args[1]) {
        if (const auto status = nanosleep(&delay, nullptr)) return status;
        const auto size = read(STDIN_FILENO, bytes, sizeof(bytes));
        if (size < 0) return size;
        if (size == 0) break;
        count += size;
    }
    output out;
    (void)libk::fmt::format_to<"bytes: {}\n">(out, count);
    return 0;
}
