#include <ipc.hpp>
#include <util.hpp>
#include <libk/parse.hpp>

int main(int argc, char** argv) {
    if (argc != 2) return STATUS_BAD_ARGS;
    const auto milliseconds = libk::parse<uint64_t>(argv[1]);
    if (!milliseconds) return STATUS_BAD_ARGS;
    if (!*milliseconds) return STATUS_OK;
    const auto deadline = rt_after_ms(*milliseconds);
    if (!deadline) return STATUS_BAD_ARGS;
    for (;;) {
        const auto status = ev_wait(rt_events(), *deadline).status;
        if (status != STATUS_OK) return status == STATUS_TIMED_OUT ? STATUS_OK : status;
    }
}
