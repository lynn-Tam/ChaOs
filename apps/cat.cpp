#include <vector>
#include <posix.hpp>
#include <start.hpp>
#include <unistd.h>
#include <cerrno>

int main(int argc, char** argv) {
    if (const auto rc = px_init(); rc != STATUS_OK) return rc;

    if (argc > 2) return 1;
    const int fd = argc == 1 ? STDIN_FILENO : open(argv[1], O_RDONLY);
    if (fd < 0) return errno;
    std::vector<char> buffer(io_limit);
    for (;;) {
        const auto count = read(fd, buffer.data(), buffer.size());
        if (count < 0) return errno;
        if (count == 0) break;
        if (write(STDOUT_FILENO, buffer.data(), count) != count) return 1;
    }
    return fd == STDIN_FILENO || close(fd) == 0 ? 0 : 1;
}
