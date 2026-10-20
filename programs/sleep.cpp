#include <posix.hpp>
#include <start.hpp>
#include <lib/stream/stream.hpp>
#include <time.h>
#include <uapi/abi.h>
#include <libk/parse.hpp>

int main(int argc, char** argv) {
    if (const auto rc = px_init(); rc != STATUS_OK) return rc;

    if (argc != 2) return STATUS_BAD_ARGS;
    const auto parsed = libk::parse<uint64_t>(argv[1]);
    if (!parsed) return STATUS_BAD_ARGS;
    const auto ms = *parsed;
    const timespec delay{static_cast<int64_t>(ms / 1000), static_cast<int64_t>(ms % 1000 * 1'000'000)};
    return nanosleep(&delay, nullptr);
}
