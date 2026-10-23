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
    std::string text; text.reserve(4096);
    errno = 0;
    while (auto* entry = readdir(dir)) {
        text += entry->d_name; text += '\n';
        if (text.size() < 4096) continue;
        if (write(STDOUT_FILENO, text.data(), text.size()) != static_cast<int64_t>(text.size())) { status = -1; break; }
        text.clear();
    }
    if (!status && !text.empty() && write(STDOUT_FILENO, text.data(), text.size()) != static_cast<int64_t>(text.size())) status = -1;
    if (!status && errno) status = -1;
    const auto closed = closedir(dir);
    return status < 0 ? status : closed;
}
