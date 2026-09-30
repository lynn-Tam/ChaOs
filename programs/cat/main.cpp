#include <unistd.h>

int main(int argc, char** argv) {
    if (argc > 2) return 1;
    const int fd = argc == 1 ? STDIN_FILENO : open(argv[1], O_RDONLY);
    if (fd < 0) return fd;
    char buffer[4096];
    for (;;) {
        const auto count = read(fd, buffer, sizeof(buffer));
        if (count < 0) return count;
        if (count == 0) break;
        if (write(STDOUT_FILENO, buffer, count) != count) return 1;
    }
    return fd == STDIN_FILENO || close(fd) == 0 ? 0 : 1;
}
