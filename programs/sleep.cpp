#include <time.h>
#include <sys/status.h>
#include <libk/parse.hpp>

int main(int argc, char** argv) {
    if (argc != 2) return invalid;
    const auto parsed = libk::parse<uint64_t>(argv[1]);
    if (!parsed) return invalid;
    const auto ms = *parsed;
    const timespec delay{static_cast<int64_t>(ms / 1000), static_cast<int64_t>(ms % 1000 * 1'000'000)};
    return nanosleep(&delay, nullptr);
}
