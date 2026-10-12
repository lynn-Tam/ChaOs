#include <dirent.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc > 2) return 1;
    auto* dir = opendir(argc == 2 ? argv[1] : "/");
    if (!dir) return 1;
    dirent entry{};
    int status{};
    while ((status = readdir(dir, &entry)) > 0) {
        uint64_t size{};
        while (entry.name[size]) ++size;
        if (write(STDOUT_FILENO, entry.name, size) != static_cast<int64_t>(size)
            || write(STDOUT_FILENO, "\n", 1) != 1) { status = -1; break; }
    }
    const auto closed = closedir(dir);
    return status < 0 ? status : closed;
}
