#include <unistd.h>

int main(int argc, char** argv) {
    const bool append = argc == 3 && argv[1][0] == '-' && argv[1][1] == 'a' && argv[1][2] == '\0';
    if (argc != 2 && !append) return 1;
    const int fd = open(argv[append ? 2 : 1], O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC));
    if (fd < 0) return fd;
    char data[4096];
    for (;;) {
        const auto count = read(STDIN_FILENO, data, sizeof(data));
        if (count < 0) return count;
        if (count == 0) break;
        if (write(fd, data, count) != count) return 1;
    }
    const int status = fsync(fd);
    const int closed = close(fd);
    return status ? status : closed;
}
