#include <cerrno>
#include <posix.hpp>
#include <start.hpp>
#include <lib/fs/fs.hpp>
#include <lib/stream/stream.hpp>
#include <dirent.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (const auto rc = px_init(); rc != STATUS_OK) return rc;

    if (argc > 2) return 1;
    auto* dir = opendir(argc == 2 ? argv[1] : ".");
    if (!dir) return 1;
    int status{};
    errno = 0;
    while (auto* entry = readdir(dir)) {
        uint64_t size{};
        while (entry->d_name[size]) ++size;
        if (write(STDOUT_FILENO, entry->d_name, size) != static_cast<int64_t>(size)
            || write(STDOUT_FILENO, "\n", 1) != 1) { status = -1; break; }
    }
    if (!status && errno) status = -1;
    const auto closed = closedir(dir);
    return status < 0 ? status : closed;
}
