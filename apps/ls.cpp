#include <cerrno>
#include <posix.hpp>
#include <start.hpp>
#include <dirent.h>
#include <unistd.h>
#include <string>

int main(int argc, char** argv) {
    if (const auto rc = px_init(); rc != STATUS_OK) return rc;

    if (argc > 2) return 1;
    auto* dir = opendir(argc == 2 ? argv[1] : ".");
    if (!dir) return 1;
    int status{};
    errno = 0;
    while (auto* entry = readdir(dir)) {
        const std::string line = std::string{entry->d_name} + '\n';
        if (write(STDOUT_FILENO, line.data(), line.size()) != static_cast<int64_t>(line.size())) { status = -1; break; }
    }
    if (!status && errno) status = -1;
    const auto closed = closedir(dir);
    return status < 0 ? status : closed;
}
